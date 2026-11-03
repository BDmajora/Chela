# Claude Code desktop parity

Reference: [Claude Code desktop documentation](https://code.claude.com/docs/en/desktop)
and [permissions](https://code.claude.com/docs/en/permissions), reviewed November 1, 2026.
The current visual target is the **October 2026 sectioned layout**, using
[the October 23 Chat capture, Desktop 3.6556.2](https://github.com/anthropics/claude-code/issues/104695)
and [the October sectioned Code organizer](https://github.com/anthropics/claude-code/issues/102118).
[Desktop 3.6556.2 section headers](https://github.com/anthropics/claude-code/issues/105628)
confirm that the sectioned layout remained current later in October. See the
[visual review](visual-review.md) for dimensions, captures, and remaining differences.

Full feature and pixel parity is **not complete**. A matching layout does not
establish matching agent behavior, and a mock server passing does not establish
live compatibility with every model.

## Completed and verified in this development pass

| Workflow | Behavior | Verification |
| --- | --- | --- |
| Chat inside Code | Separate Agent / Chat selector; conversation preserved when switching | Desktop test |
| Project-aware Chat | Directory listing and nested text-file reads, followed by a response using those results | HTTP integration and desktop tests |
| Chat permissions | No writes or commands, including unsolicited tool calls with a previous Auto selection | Unit and HTTP integration tests |
| Agent | File reads, approved writes, commands, iterative tool calls, cancellation | Existing and extended tests |
| Read-only external harness | Negotiate advertised plan/read-only mode before prompting; decline permission requests; stop if unsupported | ACP fixture tests |
| Session restoration | Save Chat/Agent, Chat/Code tab, and permission choice; preserve mode-only changes across restart | Store and desktop tests |
| Existing sessions | Retain history and default to Code / Agent / Ask; atomically save new options | Migration and rollback tests |
| Tool display | One expandable activity per call, including request, result, and failure indicator | Screenshot inspection |
| Request recovery | Connection settings and retry; no duplicate user turn; preserve an unsent draft | Desktop test |
| Desktop layout | Full-height 280px sidebar, top-rail icons, Code landing view, centered Chat greeting, responsive composer | Screenshot review at 1440×940 and 980×720 |
| Menus | Permissions, discovered models, recent folders, local environment, attachments, interaction, session actions, workspace | Keyboard desktop tests and screenshots |
| InferSSD library | SSH setup, host trust/password prompts, downloaded GGUF discovery, native single-model router, private tunnel, reconnect and refresh | Helper tests and isolated desktop transport fixture; live GPU inference still requires the user's SSH login |
| Nested menus | Agent / Chat, Group by, Sort by | Desktop keyboard navigation and screenshots |
| Accept edits | Direct file edits skip approval; commands still ask; Chat stays read only; ACP actions still ask | Unit tests, persistence round trip, desktop mode selection |
| Session actions | Rename from the title or sidebar, export, changes, delete | Rename persistence test, sidebar context-menu screenshot; existing export/delete behavior |
| Composer placement | Environment, folder, permissions, context and model below the Code prompt; Chat plus/microphone/waveform below the prompt, model at right | Wide and narrow screenshots |
| Sidebar organizer | October Code organizer; independent Pinned/Recents collapse, pins, custom sections and move menu; saved group/sort preferences; Chat filters | Nested menu screenshots, selections and restart |
| Context menus | Separate Skills, Connectors, Plugins submenus; Chat project/design menus; local skill instructions load into the draft | Menu screenshots and submitted fixture request |
| Dictation | Configured local transcription command inserts a reviewable draft; cancellation kills the process group | Real helper subprocess, cancellation and submitted transcript |
| Appearance settings | October modal proportions; working General controls, search and persistent width/motion | Native selection and resize checks |
| Chat appearance | Higher welcome composition, serif replies, inline code, list spacing, compact user bubbles and response actions | Substantial conversation at 1440×940 and 980×720 |

## Remaining acceptance work

| Area | Remaining work |
| --- | --- |
| Exact visuals | October main-frame geometry and palette are checked; proprietary fonts, settings, native dialogs, unavailable panels and several nested menu states still differ or lack a matching October capture |
| Models and agents | Test live local/remote models and real ACP versions; persistent harness sessions and advertised mode/model pickers |
| Project navigation | File browser, file mentions, context selection, additional workspace directories, file editor |
| Changes | Per-file diff UI, per-session change tracking, review annotations, undo/checkpoints |
| Terminal | Integrated interactive terminal and long-running processes |
| Sessions | Concurrent background sessions, worktree isolation, archive, true navigation history, completion notifications; pin/rename and custom sections are implemented |
| Context | Context usage display, compaction, model context limit handling |
| Agent tools | Targeted edits, richer search, structured plans and task status |
| Permissions | Persistent per-tool rules and Claude's automatic action reviewer. Bypass permissions preserves the old Auto behavior. Accept edits still asks for all commands; Claude also pre-approves some filesystem commands |
| Extensions | MCP configuration, executable skills, hooks and plugin lifecycle. Local Markdown skill instructions are supported |
| Preview | Running-app previews, browser workflows and visual verification |
| Other surfaces | Full CommonMark/tables, images, screenshot attachments, drag-and-drop context, platform packaging; built-in speech recognition, device enumeration and spoken voice conversations. Their visible controls remain disabled where unsupported |

Each workflow needs implementation, failure and cancellation handling, persistence
where relevant, automated coverage, and comparison with the pinned reference
before marking it complete. External harnesses enforce their own modes; Chela
does not provide an operating-system sandbox for those processes.
