# Desktop visual review — November 1, 2026

The target is the real Claude desktop's dark interface, using the Code landing
view and menu screenshots below. These screenshots represent more than one
desktop layout and do not identify an exact application version. This is a
visual convergence pass, not a claim of full pixel or feature parity.

[Previous desktop capture](desktop-before.png) · [Updated desktop](desktop.png) ·
[Permission menu](menu-permissions.png) · [Model menu](menu-models.png) ·
[Settings](settings.png) · [Conversation](conversation.png)

## Follow-up comparison pass

This pass adds [September 2026 desktop captures, published October 5](https://www.gianlu.it/formazione/interfaccia-claude-web-desktop-eng.html),
including close-ups of the plus menu, voice menu, organizer and Chat conversation.
The [official desktop reference](https://code.claude.com/docs/en/desktop) supplies
the current Manual/Accept edits/Plan terminology and confirms that the mode
selector belongs below the prompt. The public screenshots do not identify their
exact release, so this is still not a version-pinned pixel-parity claim.

| Comparison round | Finding and correction |
| --- | --- |
| Composer and sidebar | Moved model below the box, folder/environment above Code input, and permission/context controls below it. Replaced the sort arrow with sliders; flat Recents no longer inserts the project name. Only the active Chat/Code tab has a label. |
| Native rendering | Screenshot review exposed hidden folder controls and misplaced Chat microphone controls. Corrected visibility and placement; checked both window sizes. |
| Submenus and state | Rebuilt the plus menu around files, Skills, Connectors and Plugins; added Chat project/design entries. Added Type, Project and activity filters, Date/Project/None groups, sorting values and saved preferences. Exercised every new submenu and selection. |
| Voice lifecycle | Added microphone/waveform/options controls and the Dictate/Voice mode/Microphone menu. Real helper tests found an Escape/destroy race; retained the dialog until its asynchronous callback finishes. Verified transcript insertion and cancellation. |
| Chat composition | Comparison with the desktop Chat capture showed the greeting/composer too low. Raised the composition, removed the extra subtitle, moved microphone controls right, used serif response text, styled inline/fenced code and lists, tightened user bubbles, and moved response actions below the text. |
| Narrow conversation | [GTK's cached text layout](https://raw.githubusercontent.com/GNOME/gtk/gtk-3-24/gtk/gtktextview.c) reused its previous line width as a minimum and expanded narrow windows. Gave reply views a flexible width and refreshed their full height after allocation. Verified the final paragraph remains visible, and added a check that long replies preserve the requested 980-pixel window width. |
| Final review | Compared the expanded Chat plus menu, project/design/skills/voice/microphone submenus, model menu, organizer, session actions, and substantial Chat/Code conversations at 1440×940 and 980×720. Unit, integration and expanded desktop checks pass with GTK warnings treated as failures. |

[Previous pass](desktop-previous.png) · [Code](desktop.png) ·
[Chat welcome](chat.png) · [Chat plus menu](menu-attach.png) ·
[Voice](menu-voice.png) · [Organizer](menu-sort.png) ·
[Skills](menu-skills.png) · [Chat conversation](chat-conversation.png) ·
[Narrow Chat](chat-narrow.png) · [Narrow Code](code-narrow.png)

Dictation requires a user-configured transcription app; no recognition engine is
bundled. Spoken voice mode, screenshot/image input, research and web search remain
disabled. Connector/plugin submenus disclose that extensions are managed by an
ACP harness. Local Markdown skills and design guidelines actually load into the
draft. These distinctions are also recorded in [the parity checklist](parity.md).

## References inspected

| State | Source |
| --- | --- |
| Code landing view, sidebar, environment picker | [April 2026 desktop walkthrough](https://blog.serverworks.co.jp/claude-code-desktop-redesign-2026) |
| Compact permission menu | [Desktop permission-menu screenshot](https://www.xmsumi.com/detail/3114) |
| Chat greeting, composer, descriptive model picker | [Desktop redesign screenshots](https://ai.georgeliu.com/p/claude-ai-desktop-app-gets-redesigned) |
| Conversation spacing and compact user messages | [Desktop conversation capture](https://insight.eggman.tv/insight/articles/9) |
| Current supported control placement and permission terminology | [Official desktop reference](https://code.claude.com/docs/en/desktop) |

## Comparison loop

1. Compared the old `docs/desktop.png` with the real screenshots. Rebuilt the
   sidebar proportions, navigation, Code landing view, Chat greeting, composer,
   and menu surfaces. Re-ran the existing conversation and approval workflows.
2. Captured each menu and nested menu. Fixed a GTK submenu ownership warning,
   revealed the hidden home-session list, and split settings into Connections
   and Code pages. Verified model discovery, model selection, and session rename.
3. Compared 1440×940 and 980×720 captures. Fixed width calculations that clipped
   the composer at smaller sizes. Made the permissions menu compact and replaced
   theme-dependent icons with consistent outline icons.
4. Added sidebar session menus, constrained long folder/model labels, and checked
   persisted permission choices, read-only Chat, nested navigation, sidebar
   collapse, error recovery, cancellation, and restart.
5. Verified both sidebar menu triggers, cancellation during delayed model
   discovery, and both settings pages. Compared a real conversation capture;
   replaced full-width user cards with compact bubbles and moved the assistant
   mark and copy action below the response. Empty composers show a disabled send
   control; running requests retain an enabled stop control.

## Reproduce the captures

Run `make ui-test`. It launches an isolated virtual display, disposable settings
and session history, and a local mock model server. It does not use personal
settings or connect to a real inference server. The tests fail on GTK warnings.

| Capture in `build/` | State |
| --- | --- |
| `ui-welcome.png`, `ui-home-sessions.png` | Empty and populated Code landing views |
| `ui-chat-welcome.png`, `ui-conversation.png`, `ui-code-chat.png` | Chat landing view and conversations |
| `ui-menu-permissions.png`, `ui-menu-accept-edits.png` | Permission menu, selected mode checkmarks |
| `ui-menu-models.png` | Discovered server models, current model, connection settings |
| `ui-menu-projects.png`, `ui-menu-environment.png` | Folder and environment menus |
| `ui-menu-attach.png`, `ui-menu-skills.png`, `ui-menu-connectors.png`, `ui-menu-plugins.png` | Code context menu and extension submenus |
| `ui-chat-menu-attach.png`, `ui-chat-menu-project.png`, `ui-chat-menu-design.png` | Chat context menu, project selection and design guidelines |
| `ui-menu-voice.png`, `ui-menu-microphone.png`, `ui-voice-settings.png` | Voice options, device submenu and local dictation setup |
| `ui-menu-interaction.png` | Agent / Chat submenu in session actions |
| `ui-menu-session.png`, `ui-menu-session-row.png`, `ui-menu-session-context.png` | Title, sidebar button, and right-click session actions |
| `ui-menu-group.png`, `ui-menu-sort.png` | Nested grouping and sorting menus |
| `ui-menu-type.png`, `ui-menu-filter-project.png`, `ui-menu-activity.png` | Type, project and last-activity filters |
| `ui-grouped-date.png`, `ui-grouped-project.png`, `ui-recents-flat.png` | Applied groups and flat Recents |
| `ui-chat-rich.png`, `ui-chat-rich-narrow.png`, `ui-code-rich.png`, `ui-code-rich-narrow.png` | Substantial replies at both window sizes |
| `ui-menu-workspace.png`, `ui-settings.png`, `ui-settings-code.png` | Workspace menu and both settings pages |
| `ui-narrow.png`, `ui-sidebar-hidden.png` | Narrow window and collapsed sidebar |
| `ui-approval.png`, `ui-connection-error.png` | Approval and recovery states |

Popup screenshots capture the virtual display so separate native menu windows
are included. Their extra black margin is outside the application window.

The model fixtures provide `stream` and `tool`; these are real selectable fixture
IDs, not labels imitating unavailable Claude models. Unsupported cloud, worktree,
subscription and scheduling controls are omitted. Visible extension and voice
menus disclose unsupported functionality. Remaining work is tracked in
[the parity checklist](parity.md).
