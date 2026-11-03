# October 2026 desktop comparison

The target selected for this pass is the **current October 2026 layout**.
The earlier pass mixed the April Code layout and September Chat layout. This
pass replaces their structural differences, rather than treating more screenshots
or passing functional tests as evidence of a pixel match.

[Code](desktop.png) · [Chat](chat-october.png) · [Chat welcome](chat.png) ·
[Appearance settings](settings-general.png) · [Code organizer](menu-sort.png) ·
[Plus menu](menu-attach.png) · [Voice menu](menu-voice.png) ·
[Narrow conversation](chat-narrow.png)

## Reference baseline

- [October 23 desktop 3.6556.2: Chat and Settings](https://github.com/anthropics/claude-code/issues/104695).
- [October sectioned Code organizer](https://github.com/anthropics/claude-code/issues/102118):
  Group by, Sort by, Show, New section, View all. The other screenshot in that
  report is explicitly the old organizer and is not the target.
- [Desktop 3.6556.2 section headers](https://github.com/anthropics/claude-code/issues/105628)
  confirm the sectioned Code layout continued later in October.
- [Official control placement](https://code.claude.com/docs/en/desktop):
  environment, folder, permissions, and model below the prompt.

The Chat reference is 1917×1078. For logical-pixel comparisons it is normalized
by 0.8 to approximately 1534×862. That scale is inferred from the raster's widget
sizes; the source does not publish its display DPI. The Code organizer reference
already has a roughly 280-pixel sidebar and is inspected at its original scale.
Source URLs and image hashes are in [the reference manifest](reference-manifest.json).

## Corrections made

| Area | Result |
| --- | --- |
| Window frame | Full-height 280px sidebar; removed the inset rounded card and warm palette. Hamburger, collapse, previous/next, and icon-only Chat/Code controls share the top rail. |
| Sidebar | Restored New, Search, Projects, Artifacts, Scheduled, Customize, and More rows. Added separate collapsible Pinned and Recents, functional pin/unpin, custom sections and Move to. The entire history area scrolls together. Flat Recents does not insert the project name. |
| Prompt | Environment, folder, permissions and model are below Code input. Chat has plus, dictation, voice and options below input, with model on the right. Replaced the colored arrow with the return control and reduced single-line input from 52px to 44px. |
| Conversation | Visible title and caret in the top rail, Chat Share/export action, compact user messages, serif Chat replies, sans-serif Code replies, copy/read-aloud/feedback/retry row. Removed the obsolete star after every reply. |
| Menus | Current Code organizer, nested grouping/sorting/show options, section creation and moves. Corrected plus labels/icons and right alignment of model/voice menus. |
| Settings | Replaced the small native form with an inset 1024px modal, Settings header and close control, 212px navigation column, search, and appearance rows. Font, response font, transcript size/width and motion choices persist. Connections and Code retain their working model settings. |

The comparison rounds caught hidden conversation-title children, invisible pinned
rows, oversized organizer rows, incorrect prompt height, and history sections
outside the scroll area. Each was corrected and recaptured. The native test now
exercises pin/unpin, collapse, actual section moves, settings search, width and
motion selection, and response retry in addition to the existing workflows.

## Measured frame

These are direct measurements from the final rendered app, not widget requests.
The reference prompt is approximately `(524, 780, 768, 43)` after normalization;
Chela renders `(523, 780, 768, 44)` at 1534×862.

| Item | Final value |
| --- | --- |
| Sidebar / conversation / input colors | `#1f1f1f` / `#191919` / `#303030`, matching the October raster samples |
| Sidebar width | 280px |
| Prompt at 1534×862 | x=523, y=780, width=768, height=44 |
| Prompt at 980×720 | width=636, height=44, centered in the available area |
| Prompt with sidebar hidden at 980×720 | width=768, height=44, centered |

[Recorded frame measurements](visual-geometry.json). The desktop test fails if
the default palette, prompt width, height, or centering drifts. It also verifies
that a selected Medium transcript actually renders at 896px before restoring
the default width.

## Verification and limits

`make test`, `make integration`, and `make ui-test` pass. The desktop tests render
with the headless backend, which uses the same cairo drawing and text code as the
native Wayland backend, so the measured frames are the ones a Wayland session shows.
The build completed without compiler warnings. Tests use isolated settings,
history and a local model fixture, and do not change personal settings.

**Complete 1:1 pixel parity is not certified.** The checked frame coordinates are
within one logical pixel of the normalized Chat reference. Matching those values
does not prove every screen matches. Anthropic's proprietary fonts are not
bundled; installed Anthropic fonts are preferred for interface/response text,
with system fallbacks. Several nested menus still lack an October reference.
Settings offers Chela's supported pages and controls rather than subscription
and account services. Native file/approval dialogs and unavailable panels still
differ. Model labels show real configured models, not unavailable Claude models.

Artifacts, scheduling, image input, spoken voice mode and read-aloud are not
implemented. Their visible controls disclose their unavailable state. Dictation
still uses the configured local transcription app. Feedback marks apply within
the current view; Share exports a file. Retry is restricted to the latest direct
text response and cannot replay tools or external harness actions.

[Previous comparison report](visual-review-previous.md) ·
[Before this pass](desktop-before-october.png) · [Earlier Chat](chat-before-october.png)
