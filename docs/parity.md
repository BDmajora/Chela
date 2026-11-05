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
| Project-aware Chat | In a Code session: directory listing and nested text-file reads, followed by a response using those results | HTTP integration and desktop tests |
| Separate Chat and Code | Self-contained chats with no folder or tools; each tab keeps its own sessions, search and draft | Requests inspected in `tests/workspace_ui.py` |
| Folders | Blank by default; No folder, Recent, Open folder; Add folder and Also included, kept with the session; file tools refuse everything else | Unit tests of each escape, requests and real tool calls in `tests/workspace_ui.py` |
| Command sandbox | Landlock limits what commands write to the session's folders, temporary folders and package caches; no setuid; refuses unattended commands where unavailable | Each escape attempted in `tests/test_core.c` with real commands; real gcc, make, git and Python runs |
| Chat permissions | No writes or commands, including unsolicited tool calls with a previous Auto selection | Unit and HTTP integration tests |
| Agent | File reads, approved writes, commands, iterative tool calls, cancellation | Existing and extended tests |
| Read-only external harness | Negotiate advertised plan/read-only mode before prompting; decline permission requests; stop if unsupported | ACP fixture tests |
| Auto | Rules decide: edits and sandboxed commands run; pushing, publishing, deleting folders, discarding git work, posting data, running downloads, other users, services outside the sandbox and protected files ask, with the reason; commands nothing can confine run only if they change nothing | Command table and real tool runs in `tests/test_core.c`, real approvals in `tests/workspace_ui.py` |
| Harness permission modes | Each mode selects the harness's matching advertised mode; Chela answers each permission request by kind, command and file location, whether or not one matched | Every mode against 13 kinds of action in ACP fixture tests |
| Mode switcher | Auto, Manual, Accept edits, Plan, and Bypass behind Enable with a confirmation, off again in Settings; 1 to 5 choose; new sessions start in the last choice, never Bypass after a restart | Desktop tests and screenshots against the reference |
| Session restoration | Save Chat/Agent, Chat/Code tab, and permission choice; preserve mode-only changes across restart | Store and desktop tests |
| Existing sessions | Retain history and default to Code / Agent / Ask; atomically save new options | Migration and rollback tests |
| Tool display | Consecutive calls fold into one line ("Ran 2 commands, read 3 files (1 failed)"); each opens to its request and result | Screenshot inspection |
| Markdown | Headings, emphasis, nested and task lists, quotes, tables, rules, links, code blocks with copy; safe on partial text while streaming | C unit tests, including every prefix of a document and random bytes |
| Attachments | Paste and drop of images and files, previews, sending to the model, restored transcripts, clean-up | C unit tests, desktop test, paste and drop through the real Wayland backend in nested KWin |
| Images on a model server | Pair a validated vision file with each model and start it with `--mmproj`, reserve GPU memory for the encoder, download it with the weights, say so before sending to a model without one | Helper and downloader tests, the real router's command line, desktop test. Not yet observed reading an image on a real GPU |
| Model residency | Pre-load and idle unload as separate settings, one switch in the model menu, specifics in Settings | Desktop test with a fixture router, including migration from keep-loaded |
| Context use | Percentage under the composer, tokens in the popover and environment panel, from the server's own context size and usage | C unit tests, desktop test, screenshot review |
| Load progress | Measured percentage, stage and ETA from the inference computer, learned from earlier loads | C unit tests, helper tests, fixture server. Not yet observed against a real GPU load |
| Environment panel | This computer, project, branch, context use and changes for a Code session | Screenshot review |
| Branch and worktree | Folder menu Branch row with a switching submenu that refuses uncommitted work; Worktree box that runs a new session in its own git worktree, saved with the session; deleting the session offers to remove the worktree and branch, and never takes uncommitted or unmerged work | `tests/workspace_ui.py` against real git repositories |
| Request recovery | Connection settings and retry; no duplicate user turn; preserve an unsent draft | Desktop test |
| Desktop layout | Full-height 280px sidebar, top-rail icons, Code landing view, centered Chat greeting, responsive composer | Screenshot review at 1440×940 and 980×720 |
| Menus | Permissions, discovered models, recent folders, local environment, attachments, interaction, session actions, workspace | Keyboard desktop tests and screenshots |
| InferSSD library | SSH setup, host trust/password prompts, downloaded GGUF discovery, native single-model router, private tunnel, reconnect and refresh | Helper tests and isolated desktop transport fixture; live GPU inference still requires the user's SSH login |
| Nested menus | Agent / Chat, Group by, Sort by | Desktop keyboard navigation and screenshots |
| Accept edits | Direct file edits skip approval; commands still ask; Chat stays read only; ACP edits to named files in the session are accepted, other ACP actions ask | Unit tests, persistence round trip, desktop mode selection, ACP fixture tests |
| Session actions | Rename from the title or sidebar, export, changes, archive, delete. Right click does not open the session unless the action needs it | Rename persistence test, sidebar context-menu screenshot; existing export/delete behavior |
| Folder headings | Right click: New session, Move up and down, Select all, Collapse all, Expand all, Archive all, Delete all sessions. Save as environment is left out: environments have nothing behind them yet | Desktop test of each item, screenshot against the reference |
| Several sessions | Ctrl+click and Select all mark sessions; archive or delete them together; Esc clears | Desktop test |
| Archive | Out of the sidebar, kept in View all, Restore session brings it back | Store round trip and migration, desktop test |
| Composer placement | Plus and microphone in the prompt; environment and folder at left below a new Code session; model, thinking level, permission mode and status ring at right. Settings moved to the Settings dialog | Wide and narrow screenshots |
| Sidebar organizer | October Code organizer; independent Pinned/Recents collapse, pins, custom sections and move menu; saved group/sort preferences | Nested menu screenshots, selections and restart |
| Context menus | Add files and, in Code, Add folder and Skills; local skill instructions load into the draft. Controls with nothing behind them were removed | Menu screenshots and submitted fixture request |
| Dictation | Configured local transcription command inserts a reviewable draft; cancellation kills the process group | Real helper subprocess, cancellation and submitted transcript |
| Appearance settings | October modal proportions; working General controls, search and persistent width | Native selection and resize checks |
| Chat appearance | Higher welcome composition, serif replies, inline code, list spacing, compact user bubbles and response actions | Substantial conversation at 1440×940 and 980×720 |

## Remaining acceptance work

| Area | Remaining work |
| --- | --- |
| Exact visuals | October main-frame geometry and palette are checked; proprietary fonts, settings, native dialogs, unavailable panels and several nested menu states still differ or lack a matching October capture |
| Models and agents | Test live local/remote models and real ACP versions; persistent harness sessions and advertised mode/model pickers |
| Project navigation | File browser, file mentions, context selection, file editor |
| Changes | Per-file diff UI, per-session change tracking, review annotations, undo/checkpoints |
| Terminal | Integrated interactive terminal and long-running processes |
| Sessions | Concurrent background sessions, true navigation history, completion notifications, saved environments; pin/rename, custom sections and archive are implemented |
| Context | Compaction, model context limit handling. Usage shows in the status ring and environment panel |
| Agent tools | Targeted edits, richer search, structured plans and task status |
| Permissions | Persistent per-tool rules, and a model that reviews actions: Auto here decides by rules. Changing mode during a run. Accept edits still asks for all commands; Claude also pre-approves some filesystem commands |
| Extensions | MCP configuration, executable skills, hooks and plugin lifecycle. Local Markdown skill instructions are supported |
| Preview | Running-app previews, browser workflows and visual verification |
| Other surfaces | Reference-style links and inline HTML, a built-in screenshot tool, platform packaging; built-in speech recognition, device enumeration and spoken voice conversations. Artifacts, Scheduled, Connectors, Plugins and a light theme have no controls until they work |

Each workflow needs implementation, failure and cancellation handling, persistence
where relevant, automated coverage, and comparison with the pinned reference
before marking it complete. External harnesses enforce their own modes, and can act
without asking; Chela decides what they do ask about, and does not provide an
operating-system sandbox for those processes.
