/* Qt 6 host, loaded as a plugin with --backend=qt (and used on X11 when no
 * Wayland compositor is running). Qt supplies the window, input, input methods,
 * the clipboard, the main loop and native file dialogs, and draws with QPainter
 * and Qt's own text engine. Needs no moc. */
extern "C" {
#include "ui/platform.h"
}
#include <QBuffer>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>
#include <QApplication>
#include <QClipboard>
#include <QFileDialog>
#include <QFont>
#include <QFontMetricsF>
#include <QHash>
#include <QIcon>
#include <QInputMethod>
#include <QKeyEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QWidget>
#include <QWindow>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

struct CuImage {
    QImage image;
};
struct CuCanvas {
    QPainter *painter;
    QPainterPath path;
};
struct CuFont {
    CuFontDesc desc;
    std::string family;
    QFont font;
    QFontMetricsF metrics;
    CuFontMetrics m;
    QHash<QByteArray, qreal> widths; /* measured strings; cleared when large */
};

namespace {
QApplication *app;
bool running, quit_pending;
std::vector<CuFont *> fonts;
std::unordered_map<unsigned, QTimer *> timers;
unsigned next_timer;

QColor color(CuColor c) {
    return QColor::fromRgba(QRgb(c));
}
QString utf8(const char *text, size_t length) {
    return QString::fromUtf8(text, (qsizetype)length);
}
int mods_from(Qt::KeyboardModifiers m) {
    int mods = 0;
    if (m & Qt::ShiftModifier)
        mods |= CU_MOD_SHIFT;
    if (m & Qt::ControlModifier)
        mods |= CU_MOD_CTRL;
    if (m & Qt::AltModifier)
        mods |= CU_MOD_ALT;
    if (m & Qt::MetaModifier)
        mods |= CU_MOD_SUPER;
    return mods;
}
int key_from(int key) {
    switch (key) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        return CU_KEY_RETURN;
    case Qt::Key_Escape:
        return CU_KEY_ESCAPE;
    case Qt::Key_Tab:
    case Qt::Key_Backtab:
        return CU_KEY_TAB;
    case Qt::Key_Backspace:
        return CU_KEY_BACKSPACE;
    case Qt::Key_Delete:
        return CU_KEY_DELETE;
    case Qt::Key_Left:
        return CU_KEY_LEFT;
    case Qt::Key_Right:
        return CU_KEY_RIGHT;
    case Qt::Key_Up:
        return CU_KEY_UP;
    case Qt::Key_Down:
        return CU_KEY_DOWN;
    case Qt::Key_Home:
        return CU_KEY_HOME;
    case Qt::Key_End:
        return CU_KEY_END;
    case Qt::Key_PageUp:
        return CU_KEY_PAGE_UP;
    case Qt::Key_PageDown:
        return CU_KEY_PAGE_DOWN;
    case Qt::Key_Insert:
        return CU_KEY_INSERT;
    case Qt::Key_Menu:
        return CU_KEY_MENU;
    default:
        break;
    }
    if (key >= Qt::Key_F1 && key <= Qt::Key_F12)
        return CU_KEY_F1 + (key - Qt::Key_F1);
    /* Printable keys are their (upper-case) code points. */
    if (key >= 0x20 && key < 0x110000 && key != 0x7f)
        return (int)QChar::toLower((char32_t)key);
    return 0;
}
} // namespace

/* Files first, then text, then pictures: the same order as ch_clipboard_best. */
static const char *mime_payload(const QMimeData *m, QByteArray *out) {
    if (!m)
        return nullptr;
    if (m->hasUrls()) {
        for (const QUrl &url : m->urls())
            *out += url.toEncoded() + "\r\n";
        return "text/uri-list";
    }
    if (m->hasText() && !m->text().isEmpty()) {
        *out = m->text().toUtf8();
        return "text/plain";
    }
    if (m->hasImage()) {
        QBuffer buffer(out);
        buffer.open(QIODevice::WriteOnly);
        if (qvariant_cast<QImage>(m->imageData()).save(&buffer, "PNG"))
            return "image/png";
    }
    return nullptr;
}
struct CuWindow final : QWidget {
    const CuWindowHandler *handler;
    void *data;
    bool csd, starting = true, text_input = false;
    QRectF caret;

    CuWindow(const CuWindowHandler *h, void *d, bool client_decorations)
        : handler(h), data(d), csd(client_decorations) {
        setAttribute(Qt::WA_OpaquePaintEvent);
        setAttribute(Qt::WA_NoSystemBackground);
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);
        setAcceptDrops(true);
        if (csd)
            setWindowFlag(Qt::FramelessWindowHint);
    }
    void send(CuEvent ev) {
        if (!starting)
            handler->event(this, &ev, data);
    }
    void pointer(CuEventType type, QMouseEvent *e) {
        Qt::MouseButton button = e->button();
        int b = button == Qt::LeftButton ? 1 : button == Qt::MiddleButton ? 2 : button == Qt::RightButton ? 3 : 0;
        if (type != CU_EV_MOTION && !b)
            return;
        CuEvent ev{};
        ev.type = type;
        ev.x = (float)e->position().x();
        ev.y = (float)e->position().y();
        ev.button = b;
        ev.mods = mods_from(e->modifiers());
        send(ev);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        if (starting)
            return;
        QPainter painter(this);
        painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing |
                               QPainter::SmoothPixmapTransform);
        CuCanvas canvas{&painter, QPainterPath()};
        canvas.path.setFillRule(Qt::WindingFill);
        handler->paint(this, &canvas, width(), height(), data);
    }
    void resizeEvent(QResizeEvent *) override {
        CuEvent ev{};
        ev.type = CU_EV_RESIZE;
        ev.width = width();
        ev.height = height();
        ev.scale = (float)devicePixelRatioF();
        send(ev);
    }
    void mouseMoveEvent(QMouseEvent *e) override {
        pointer(CU_EV_MOTION, e);
    }
    /* The interface counts clicks itself, so a double click is another press. */
    void mousePressEvent(QMouseEvent *e) override {
        pointer(CU_EV_PRESS, e);
    }
    void mouseDoubleClickEvent(QMouseEvent *e) override {
        pointer(CU_EV_PRESS, e);
    }
    void mouseReleaseEvent(QMouseEvent *e) override {
        pointer(CU_EV_RELEASE, e);
    }
    void wheelEvent(QWheelEvent *e) override {
        /* Touchpads report pixels; one wheel notch scrolls about three lines, as in the Wayland backend. */
        QPointF d = !e->pixelDelta().isNull() ? QPointF(e->pixelDelta())
                                               : QPointF(e->angleDelta()) / 120.0 * 56.0;
        CuEvent ev{};
        ev.type = CU_EV_SCROLL;
        ev.x = (float)e->position().x();
        ev.y = (float)e->position().y();
        ev.dx = (float)-d.x();
        ev.dy = (float)-d.y();
        ev.mods = mods_from(e->modifiers());
        if (ev.dx || ev.dy)
            send(ev);
    }
    void leaveEvent(QEvent *) override {
        CuEvent ev{};
        ev.type = CU_EV_LEAVE;
        send(ev);
    }
    void dragEnterEvent(QDragEnterEvent *e) override {
        e->acceptProposedAction();
        dragged(true, e->position());
    }
    void dragMoveEvent(QDragMoveEvent *e) override {
        e->acceptProposedAction();
        dragged(true, e->position());
    }
    void dragLeaveEvent(QDragLeaveEvent *) override {
        dragged(false, QPointF());
    }
    void dropEvent(QDropEvent *e) override {
        QByteArray bytes;
        const char *mime = mime_payload(e->mimeData(), &bytes);
        CuEvent ev{};
        ev.type = CU_EV_DROP;
        ev.mime = mime;
        ev.data = bytes.constData();
        ev.size = (size_t)bytes.size();
        if (mime)
            send(ev);
        e->acceptProposedAction();
    }
    void dragged(bool inside, QPointF at) {
        CuEvent ev{};
        ev.type = CU_EV_DRAG;
        ev.focused = inside;
        ev.x = (float)at.x();
        ev.y = (float)at.y();
        send(ev);
    }
    bool focusNextPrevChild(bool) override {
        return false; /* Tab belongs to the interface */
    }
    void keyPressEvent(QKeyEvent *e) override {
        CuEvent ev{};
        int mods = mods_from(e->modifiers()), k = key_from(e->key());
        if (k) {
            ev.type = CU_EV_KEY;
            ev.key = k;
            ev.mods = mods;
            ev.repeat = e->isAutoRepeat();
            send(ev);
        }
        QString text = e->text();
        if (!text.isEmpty() && text.at(0).unicode() >= 0x20 && text.at(0).unicode() != 0x7f &&
            !(mods & (CU_MOD_CTRL | CU_MOD_ALT | CU_MOD_SUPER))) {
            QByteArray bytes = text.toUtf8();
            ev = CuEvent{};
            ev.type = CU_EV_TEXT;
            ev.text = bytes.constData();
            send(ev);
        }
    }
    void inputMethodEvent(QInputMethodEvent *e) override {
        CuEvent ev{};
        if (!e->commitString().isEmpty()) {
            QByteArray commit = e->commitString().toUtf8();
            ev.type = CU_EV_TEXT;
            ev.text = commit.constData();
            send(ev);
        }
        QByteArray preedit = e->preeditString().toUtf8();
        ev = CuEvent{};
        ev.type = CU_EV_PREEDIT;
        ev.text = preedit.constData();
        send(ev);
    }
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override {
        if (query == Qt::ImEnabled)
            return text_input;
        if (query == Qt::ImCursorRectangle)
            return caret.toRect();
        return QWidget::inputMethodQuery(query);
    }
    void changeEvent(QEvent *e) override {
        if (e->type() == QEvent::ActivationChange) {
            CuEvent ev{};
            ev.type = CU_EV_FOCUS;
            ev.focused = isActiveWindow();
            send(ev);
        }
        QWidget::changeEvent(e);
    }
    void closeEvent(QCloseEvent *e) override {
        e->ignore(); /* the application decides */
        CuEvent ev{};
        ev.type = CU_EV_CLOSE;
        send(ev);
    }
};

namespace {
bool init(int *argc, char ***argv) {
    if (app)
        return true;
    /* QApplication aborts without a display; report it like the other backends instead. */
    if (!getenv("WAYLAND_DISPLAY") && !getenv("WAYLAND_SOCKET") && !getenv("DISPLAY") &&
        !getenv("QT_QPA_PLATFORM"))
        return false;
    app = new QApplication(*argc, *argv); /* keeps a reference to main's argc */
    QApplication::setQuitOnLastWindowClosed(false);
    return true;
}
void run() {
    if (!quit_pending) {
        running = true;
        QApplication::exec();
        running = false;
    }
    quit_pending = false;
}
void quit() {
    if (running)
        QApplication::quit();
    else
        quit_pending = true;
}
void shutdown_backend() {
    for (auto &t : timers)
        delete t.second;
    timers.clear();
    for (CuFont *f : fonts)
        delete f;
    fonts.clear();
    delete app;
    app = nullptr;
}

CuWindow *window_new(const char *title, const char *app_id, int width, int height, bool client_decorations,
                     const CuWindowHandler *handler, void *data) {
    const char *mode = getenv("CHELA_DECORATIONS");
    bool csd = client_decorations && !(mode && !strcmp(mode, "server"));
    QGuiApplication::setDesktopFileName(QString::fromUtf8(app_id)); /* the Wayland app id */
    auto *w = new CuWindow(handler, data, csd);
    w->setWindowTitle(QString::fromUtf8(title));
    w->setWindowIcon(QIcon::fromTheme(QString::fromUtf8(app_id)));
    w->resize(width, height);
    w->show();
    w->starting = false;
    return w;
}
void window_free(CuWindow *w) {
    delete w;
}
void window_set_title(CuWindow *w, const char *title) {
    w->setWindowTitle(QString::fromUtf8(title));
}
void window_set_min_size(CuWindow *w, int width, int height) {
    w->setMinimumSize(width, height);
}
void window_redraw(CuWindow *w) {
    w->update();
}
void window_set_cursor(CuWindow *w, CuCursor cursor) {
    static const Qt::CursorShape shapes[] = {Qt::ArrowCursor,    Qt::PointingHandCursor, Qt::IBeamCursor,
                                             Qt::SizeAllCursor,  Qt::SizeVerCursor,      Qt::SizeVerCursor,
                                             Qt::SizeHorCursor,  Qt::SizeHorCursor,      Qt::SizeBDiagCursor,
                                             Qt::SizeFDiagCursor, Qt::SizeFDiagCursor,   Qt::SizeBDiagCursor};
    if ((size_t)cursor < sizeof(shapes) / sizeof(shapes[0]) && w->cursor().shape() != shapes[cursor])
        w->setCursor(shapes[cursor]);
}
void window_minimize(CuWindow *w) {
    w->showMinimized();
}
bool window_is_maximized(CuWindow *w) {
    return w->isMaximized() || w->isFullScreen();
}
void window_toggle_maximize(CuWindow *w) {
    if (window_is_maximized(w))
        w->showNormal();
    else
        w->showMaximized();
}
void window_begin_move(CuWindow *w) {
    if (w->windowHandle())
        w->windowHandle()->startSystemMove();
}
void window_begin_resize(CuWindow *w, CuEdge edge) {
    static const Qt::Edges edges[] = {Qt::Edges(),
                                      Qt::TopEdge,
                                      Qt::BottomEdge,
                                      Qt::RightEdge,
                                      Qt::LeftEdge,
                                      Qt::TopEdge | Qt::RightEdge,
                                      Qt::TopEdge | Qt::LeftEdge,
                                      Qt::BottomEdge | Qt::RightEdge,
                                      Qt::BottomEdge | Qt::LeftEdge};
    if (edge && (size_t)edge < sizeof(edges) / sizeof(edges[0]) && w->windowHandle())
        w->windowHandle()->startSystemResize(edges[edge]);
}
bool window_has_client_decorations(CuWindow *w) {
    return w->csd;
}
void window_set_text_input(CuWindow *w, bool enabled, CuRect caret) {
    QRectF rect(caret.x, caret.y, 1, caret.h);
    if (enabled == w->text_input && rect == w->caret)
        return;
    bool toggled = enabled != w->text_input;
    w->text_input = enabled;
    w->caret = rect;
    w->setAttribute(Qt::WA_InputMethodEnabled, enabled);
    if (w->hasFocus())
        QGuiApplication::inputMethod()->update(toggled ? Qt::ImQueryAll : Qt::ImCursorRectangle);
}

/* Main loop */
void post(void (*fn)(void *), void *data) {
    QMetaObject::invokeMethod(app, [fn, data] { fn(data); }, Qt::QueuedConnection); /* thread-safe */
}
unsigned timer_add(int ms, bool (*fn)(void *), void *data) {
    auto *t = new QTimer();
    unsigned id = ++next_timer;
    t->setInterval(std::max(ms, 0));
    t->setTimerType(ms < 100 ? Qt::PreciseTimer : Qt::CoarseTimer);
    QObject::connect(t, &QTimer::timeout, [id, fn, data] {
        bool again = fn(data);
        auto it = timers.find(id);
        if (it == timers.end() || again)
            return; /* removed by the callback, or repeating */
        it->second->stop();
        it->second->deleteLater();
        timers.erase(it);
    });
    timers[id] = t;
    t->start();
    return id;
}
void timer_remove(unsigned id) {
    auto it = timers.find(id);
    if (it == timers.end())
        return;
    it->second->stop();
    it->second->deleteLater();
    timers.erase(it);
}

/* Clipboard */
void clipboard_set(CuWindow *, const char *text) {
    QGuiApplication::clipboard()->setText(QString::fromUtf8(text));
}
void clipboard_request(CuWindow *, void (*callback)(const char *, void *), void *data) {
    QString text = QGuiApplication::clipboard()->text();
    if (text.isEmpty())
        callback(nullptr, data);
    else
        callback(text.toUtf8().constData(), data);
}

void clipboard_request_data(CuWindow *, void (*callback)(const char *, const void *, size_t, void *), void *data) {
    QByteArray bytes;
    const char *mime = mime_payload(QGuiApplication::clipboard()->mimeData(), &bytes);
    callback(mime, mime ? bytes.constData() : nullptr, (size_t)bytes.size(), data);
}

/* Text */
CuFont *font_get(const CuFontDesc *desc) {
    std::string family = desc->family && *desc->family ? desc->family : "sans-serif";
    for (CuFont *f : fonts)
        if (f->desc.size == desc->size && f->desc.weight == desc->weight && f->desc.italic == desc->italic &&
            f->desc.monospace == desc->monospace && f->family == family)
            return f;
    QStringList families;
    for (QString name : QString::fromStdString(family).split(',')) {
        name = name.trimmed();
        if (name.size() >= 2 && (name.front() == '\'' || name.front() == '"'))
            name = name.mid(1, name.size() - 2);
        if (!name.isEmpty())
            families << name;
    }
    if (desc->monospace)
        families << "monospace";
    QFont font;
    font.setFamilies(families);
    font.setStyleHint(desc->monospace ? QFont::Monospace : QFont::SansSerif);
    font.setPixelSize(std::max(1, (int)std::lround(desc->size)));
    font.setWeight(QFont::Weight(std::clamp(desc->weight, 100, 900)));
    font.setItalic(desc->italic);
    auto *f = new CuFont{*desc, family, font, QFontMetricsF(font), {}, {}};
    f->desc.family = f->family.c_str();
    f->m.ascent = (float)f->metrics.ascent();
    f->m.descent = (float)f->metrics.descent();
    f->m.height = std::ceil(f->m.ascent + f->m.descent);
    fonts.push_back(f);
    return f;
}
CuFontMetrics font_metrics(CuFont *f) {
    return f->m;
}
float text_width(CuFont *f, const char *text, size_t length) {
    QByteArray key = QByteArray::fromRawData(text, (qsizetype)length);
    auto it = f->widths.constFind(key);
    if (it != f->widths.constEnd())
        return (float)*it;
    qreal width = f->metrics.horizontalAdvance(utf8(text, length));
    if (f->widths.size() > 8192)
        f->widths.clear();
    f->widths.insert(QByteArray(text, (qsizetype)length), width);
    return (float)width;
}

/* Canvas */
void cv_save(CuCanvas *c) {
    c->painter->save();
}
void cv_restore(CuCanvas *c) {
    c->painter->restore();
}
void cv_clip(CuCanvas *c, CuRect r) {
    c->painter->setClipRect(QRectF(r.x, r.y, r.w, r.h), Qt::IntersectClip);
}
void cv_translate(CuCanvas *c, float x, float y) {
    c->painter->translate(x, y);
}
void cv_scale(CuCanvas *c, float x, float y) {
    c->painter->scale(x, y);
}
QPainterPath rounded(CuRect r, float radius) {
    QPainterPath path;
    radius = std::min(radius, std::min(r.w, r.h) / 2);
    if (radius <= 0.01f)
        path.addRect(QRectF(r.x, r.y, r.w, r.h));
    else
        path.addRoundedRect(QRectF(r.x, r.y, r.w, r.h), radius, radius);
    return path;
}
void cv_fill_rect(CuCanvas *c, CuRect r, float radius, CuColor col) {
    if (!(col >> 24) || r.w <= 0 || r.h <= 0)
        return;
    if (radius <= 0.01f)
        c->painter->fillRect(QRectF(r.x, r.y, r.w, r.h), color(col));
    else
        c->painter->fillPath(rounded(r, radius), color(col));
}
CuImage *cv_image_new(const uint32_t *pixels, int width, int height) {
    QImage view(reinterpret_cast<const uchar *>(pixels), width, height, width * 4, QImage::Format_ARGB32_Premultiplied);
    return new CuImage{view.copy()};
}
void cv_image_free(CuImage *image) {
    delete image;
}
void cv_draw_image(CuCanvas *c, CuImage *image, CuRect r, float radius) {
    c->painter->save();
    c->painter->setClipPath(rounded(r, radius), Qt::IntersectClip);
    c->painter->drawImage(QRectF(r.x, r.y, r.w, r.h), image->image);
    c->painter->restore();
}
void cv_stroke_rect(CuCanvas *c, CuRect r, float radius, float width, CuColor col) {
    if (!(col >> 24))
        return;
    QPen pen(color(col), width, Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin);
    c->painter->strokePath(rounded(r, radius), pen);
}
void cv_move_to(CuCanvas *c, float x, float y) {
    c->path.moveTo(x, y);
}
void cv_line_to(CuCanvas *c, float x, float y) {
    c->path.lineTo(x, y);
}
void cv_curve_to(CuCanvas *c, float x1, float y1, float x2, float y2, float x3, float y3) {
    c->path.cubicTo(x1, y1, x2, y2, x3, y3);
}
void cv_close_path(CuCanvas *c) {
    c->path.closeSubpath();
}
/* Like cairo, filling or stroking consumes the path. */
void cv_fill_path(CuCanvas *c, CuColor col) {
    c->painter->fillPath(c->path, color(col));
    c->path.clear();
    c->path.setFillRule(Qt::WindingFill);
}
void cv_stroke_path(CuCanvas *c, float width, CuColor col) {
    c->painter->strokePath(c->path, QPen(color(col), width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    c->path.clear();
    c->path.setFillRule(Qt::WindingFill);
}
void cv_draw_text(CuCanvas *c, CuFont *f, float x, float baseline, const char *text, size_t length,
                  CuColor col) {
    c->painter->setFont(f->font);
    c->painter->setPen(color(col));
    c->painter->drawText(QPointF(x, baseline), utf8(text, length));
}

/* Native file dialogs */
void file_dialog(CuWindow *parent, int kind, const char *title, const char *folder, const char *name,
                 void (*callback)(const char *path, void *data), void *data) {
    auto *dialog = new QFileDialog(parent, QString::fromUtf8(title), QString::fromUtf8(folder ? folder : ""));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    if (kind == 1) {
        dialog->setFileMode(QFileDialog::Directory);
        dialog->setOption(QFileDialog::ShowDirsOnly);
    } else if (kind == 2) {
        dialog->setFileMode(QFileDialog::AnyFile);
        dialog->setAcceptMode(QFileDialog::AcceptSave);
        if (name && *name)
            dialog->selectFile(QString::fromUtf8(name));
    } else
        dialog->setFileMode(QFileDialog::ExistingFile);
    QObject::connect(dialog, &QFileDialog::fileSelected,
                     [callback, data](const QString &path) { callback(path.toUtf8().constData(), data); });
    QObject::connect(dialog, &QFileDialog::rejected, [callback, data] { callback(nullptr, data); });
    dialog->open();
}
} // namespace

extern "C" __attribute__((visibility("default"))) const CuPlatform *chela_platform(void) {
    static CuPlatform p;
    if (!p.abi) {
        p.abi = CU_PLATFORM_ABI;
        p.name = "qt";
        p.init = init;
        p.run = run;
        p.quit = quit;
        p.shutdown = shutdown_backend;
        p.window_new = window_new;
        p.window_free = window_free;
        p.window_set_title = window_set_title;
        p.window_set_min_size = window_set_min_size;
        p.window_redraw = window_redraw;
        p.window_set_cursor = window_set_cursor;
        p.window_minimize = window_minimize;
        p.window_toggle_maximize = window_toggle_maximize;
        p.window_is_maximized = window_is_maximized;
        p.window_begin_move = window_begin_move;
        p.window_begin_resize = window_begin_resize;
        p.window_has_client_decorations = window_has_client_decorations;
        p.window_set_text_input = window_set_text_input;
        p.post = post;
        p.timer_add = timer_add;
        p.timer_remove = timer_remove;
        p.clipboard_set = clipboard_set;
        p.clipboard_request = clipboard_request;
        p.clipboard_request_data = clipboard_request_data;
        p.font_get = font_get;
        p.font_metrics = font_metrics;
        p.text_width = text_width;
        p.save = cv_save;
        p.restore = cv_restore;
        p.clip = cv_clip;
        p.translate = cv_translate;
        p.scale = cv_scale;
        p.fill_rect = cv_fill_rect;
        p.stroke_rect = cv_stroke_rect;
        p.move_to = cv_move_to;
        p.line_to = cv_line_to;
        p.curve_to = cv_curve_to;
        p.close_path = cv_close_path;
        p.fill_path = cv_fill_path;
        p.stroke_path = cv_stroke_path;
        p.draw_text = cv_draw_text;
        p.image_new = cv_image_new;
        p.image_free = cv_image_free;
        p.draw_image = cv_draw_image;
        p.file_dialog = file_dialog;
    }
    return &p;
}
