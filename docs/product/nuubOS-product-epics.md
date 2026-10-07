# nuubOS — Product Epic Backlog

> **Status:** REFINED — product backlog baseline
> **Purpose:** collect and freeze the complete set of product Epics before decomposing them into User Stories, Acceptance Criteria, Platform Requirements and implementation gaps.

## Working method

nuubOS will be refined using the following flow:

```text
Epic
  ↓
User Stories
  ↓
Acceptance Criteria
  ↓
Platform / Technical Requirements
  ↓
Current Platform Support
  ↓
GAP analysis
  ↓
Implementation / Validation
```

An Epic is not considered fully specified until its User Stories and objective acceptance criteria have been agreed.

### Epic status

| Status | Meaning |
|---|---|
| `TO REFINE` | Epic identified, scope not yet fully discussed |
| `IN REFINEMENT` | User Stories / scope currently being defined |
| `REFINED` | Scope and User Stories agreed |
| `FROZEN` | Accepted as part of the product contract |
| `DEFERRED` | Valid idea, intentionally postponed |
| `OUT OF SCOPE` | Explicitly excluded by design |

### Requirement priority

When Epics are decomposed, individual requirements will be classified as:

- **MUST**
- **SHOULD**
- **OPTIONAL**
- **DEFERRED**
- **NOT SUPPORTED BY DESIGN**

Every **MUST** requirement must eventually have an objective **PASS / FAIL** criterion.

---

# A. Core console experience

## EPIC-001 — nuubUI

**Status:** `REFINED`

### Product intent

nuubUI is the primary shell of nuubOS. It is designed from scratch as a **console-first, controller-driven** user interface.

Normal device usage must not require a touchscreen, keyboard, mouse, terminal, or knowledge of the underlying Linux system.

nuubUI is not merely a RetroArch launcher. It is the common product shell through which users access games, applications, media, streaming, settings, notifications, connectivity, power management and the other nuubOS capabilities.

Conceptually:

```text
boot
  ↓
nuubOS services
  ↓
nuubUI
  ↓
application / game
  ↓
application terminates
  ↓
return to nuubUI
```

nuubUI should remain the stable product shell while launched applications are isolated from it.

### Scope

EPIC-001 owns:

- the common console-style navigation model;
- Home information architecture;
- controller-first interaction rules;
- focus and selection behavior;
- application launch/return integration;
- integration points for notifications and background activities;
- common error/status presentation;
- handheld/TV responsive shell behavior;
- access to the Power menu;
- safe integration of advanced functionality without exposing it by default;
- shell-level crash isolation and recovery requirements.

EPIC-001 does **not** own the functional implementation of:

- Multi-user Profiles;
- Game Library;
- Settings;
- Notifications;
- Background Jobs;
- Themes;
- Controller Management;
- Wi-Fi/Bluetooth;
- Power Management;
- Updates;
- RetroArch;
- Moonlight;
- media applications;
- File Manager;
- Web Mode.

Those capabilities are implemented by their dedicated Epics and integrated into nuubUI.

### Definition of normal operation

For this Epic, a **normal/ordinary operation** is any user-facing action not explicitly classified as:

- Advanced;
- Developer;
- Diagnostics;
- Recovery;
- emergency/manual maintenance.

Every ordinary operation must be achievable using a game controller.

### Approved UX principles

1. **Controller first**
   Every ordinary function must be usable using the internal controls or a supported game controller.

2. **Console, not desktop**
   The primary experience must not expose desktop-Linux interaction concepts such as window management, taskbars or terminal-driven administration.

3. **Progressive disclosure**
   Common actions remain immediately accessible. Advanced functionality remains available but is hidden from the normal interaction path.

4. **Consistent controls**
   Confirmation, Back, contextual actions and menu actions must behave consistently throughout nuubUI.

   Default conceptual mapping:

   ```text
   A       confirm / open
   B       back / cancel
   X / Y   contextual actions
   START   menu / options
   SELECT  secondary contextual action
   ```

   Device/controller-specific presentation may adapt without breaking semantic consistency.

5. **No navigation dead ends**
   Users must always be able to understand their current location and return to a previous level or Home during ordinary navigation.

6. **One responsive handheld/TV UI**
   nuubUI must support internal displays and external TV/HDMI output without becoming two independent frontends.

7. **Offline-first core**
   Local console functionality must remain usable without Internet connectivity. Online integrations must degrade gracefully.

8. **Immediate feedback**
   Focus changes, actions, loading, progress, success and failure must produce clear feedback.

9. **Fault tolerance**
   Broken assets, unavailable online services, invalid metadata or individual application failures must not crash the product shell.

10. **Application isolation**
    A launched game/application must be able to terminate or crash without taking nuubUI down with it.

11. **Context restoration**
    Where practical, returning from a game/application should restore the previous nuubUI context rather than always returning to a generic root screen.

12. **Optional keyboard/mouse**
    Keyboard and mouse may improve specific workflows but must not be required for normal nuubUI operation.

### Approved Home hierarchy

The conceptual Home hierarchy is:

```text
Home
├── Continue / Recent
│
├── Games
│   ├── Systems
│   │   ├── PlayStation
│   │   ├── Game Boy Advance
│   │   ├── SNES
│   │   └── ...
│   │
│   └── Collections
│       ├── Favorites
│       ├── Recent
│       ├── automatic / smart collections
│       └── user-created custom collections
│
├── Applications
│   ├── Moonlight
│   ├── Steam Remote Play
│   ├── Media
│   ├── Web Mode
│   ├── File Manager
│   └── ...
│
├── Settings
│   ├── System
│   ├── Display
│   ├── Audio
│   ├── Controllers
│   ├── Wi-Fi
│   ├── Bluetooth
│   ├── Users
│   ├── Services
│   ├── Updates
│   └── ...
│
└── Power
    ├── Sleep
    ├── Restart
    └── Power Off
```

This hierarchy defines product concepts, not final visual layout.

The Power menu is a **primary console function**, not an advanced setting.

Custom game collections are first-class library objects. Their creation and management belong to EPIC-011, while nuubUI must expose them naturally alongside system and automatic collections.

### User Stories

| ID | Priority | User Story |
|---|---|---|
| `US-UI-001` | MUST | As a user, I want to use every ordinary nuubOS function using only a controller so that the device behaves like a real console. |
| `US-UI-002` | MUST | As a user, I want a clear Home from which I can quickly reach games, applications and primary system functions. |
| `US-UI-003` | MUST | As a user, I want confirmation, Back and other primary actions to behave consistently throughout the interface. |
| `US-UI-004` | MUST | As a user, I want to return easily to Home from any ordinary nuubUI section. |
| `US-UI-005` | MUST | As a user, I want to always know which UI element currently has focus. |
| `US-UI-006` | MUST | As a user, I want to launch emulators, streaming, media, Web Mode and other applications directly from nuubUI while retaining a coherent console experience. |
| `US-UI-007` | MUST | As a user, I want to leave an application and return predictably to nuubUI. |
| `US-UI-008` | MUST | As a user, I want to keep using nuubUI while scans, scraping, downloads and similar long-running operations execute in the background whenever technically possible. |
| `US-UI-009` | MUST | As a user, I want to clearly see when an activity is running, completed or failed. |
| `US-UI-010` | MUST | As a user, I want understandable and actionable error messages instead of raw Linux errors during normal use. |
| `US-UI-011` | MUST | As a normal user, I want the interface to expose only the settings I normally need while keeping advanced options available separately. |
| `US-UI-012` | MUST | As a user, I want nuubUI to remain readable and usable on both the integrated display and an external display. |
| `US-UI-013` | SHOULD | As a user, I want to optionally use keyboard and mouse in workflows that benefit from them without making them mandatory. |
| `US-UI-014` | SHOULD | As a user, I want nuubUI to restore the context I came from when I return from a game or application. |
| `US-UI-015` | SHOULD | As a user, I want smooth, understandable transitions that do not artificially slow navigation. |
| `US-UI-016` | MUST | As a user, I want the crash of a launched application to leave the main nuubUI shell usable. |
| `US-UI-017` | MUST | As a user, I want a safe recovery path if nuubUI itself cannot start correctly. |
| `US-UI-018` | MUST | As a user, I want quick and clear access to Sleep, Restart and Power Off without navigating through advanced settings. |
| `US-UI-019` | MUST | As a user, I want custom game collections to appear as first-class collections alongside built-in/automatic collections. |

### Acceptance Criteria

| User Story | PASS condition |
|---|---|
| `US-UI-001` | Every ordinary flow defined by the frozen Product Epic backlog can be completed with controller input only. No ordinary flow requires keyboard, mouse, touchscreen or terminal input. |
| `US-UI-002` | From Home, Games, Applications, Settings and Power are directly reachable through clear primary navigation; Continue/Recent content is surfaced when available. |
| `US-UI-003` | The semantic meaning of Confirm, Back/Cancel, primary menu and contextual actions remains consistent across all native nuubUI screens. Exceptions must be explicit and justified. |
| `US-UI-004` | From every native ordinary nuubUI screen there is a deterministic controller-driven path back to Home without restarting nuubUI. |
| `US-UI-005` | Any interactive native screen has exactly one unambiguous active focus target when controller navigation is expected; focus remains visually distinguishable from non-focused elements. |
| `US-UI-006` | Every supported application class can be launched from nuubUI without requiring a shell/terminal and with a defined return path to nuubUI. |
| `US-UI-007` | Normal application exit returns to a valid nuubUI state. Unexpected application termination does not strand the user at a console, blank screen or dead input state. |
| `US-UI-008` | Operations classified as background-capable do not block native nuubUI navigation for their full duration. Foreground-only operations must explicitly communicate why interaction is blocked. |
| `US-UI-009` | A background activity exposes at minimum running, success and failure states to nuubUI. Activities with measurable progress expose progress through the Notification/Background Job framework. |
| `US-UI-010` | User-facing failures provide a human-readable description and, when an action is possible, a clear recovery action. Raw technical details may exist only behind Advanced/Diagnostics. |
| `US-UI-011` | Advanced/Developer options are absent from the default primary settings path and can be deliberately entered without removing their functionality from the system. |
| `US-UI-012` | The same nuubUI product shell can render and remain fully navigable on every officially supported integrated-display target and supported external-display mode, with no clipped mandatory controls or unreadable primary text. |
| `US-UI-013` | Where keyboard/mouse support is declared for a workflow, connecting those devices provides usable input without altering the controller-only requirement for ordinary shell navigation. |
| `US-UI-014` | For application launches marked context-restorable, a normal return restores at least the previous section/item context instead of resetting navigation to Home. |
| `US-UI-015` | Animations/transitions never prevent input longer than necessary for state safety and do not introduce artificial blocking between ordinary navigation actions. |
| `US-UI-016` | Killing/crashing a launched non-shell application leaves or restores a responsive nuubUI instance without requiring device reboot. |
| `US-UI-017` | A documented recovery path exists that can restore a launchable safe/default shell when normal nuubUI startup fails because of user configuration, theme or shell state. |
| `US-UI-018` | Sleep, Restart and Power Off are reachable through a dedicated Power interaction using only a controller. Restart and Power Off require an explicit confirmation step; Sleep may execute directly. |
| `US-UI-019` | User-created custom collections supplied by the Game Library are browsable and launchable through the same Collections area used for built-in/automatic collections. |

### Dependencies

EPIC-001 depends functionally on the following Epics/Enablers but does not absorb their implementation:

| Dependency | Relationship |
|---|---|
| `EPIC-003` Multi-user Profiles | active-user identity and per-user shell state |
| `EPIC-004` Settings | native settings surfaces |
| `EPIC-006` Notification System | user-visible activity/error/status integration |
| `EPIC-007` Background Jobs & Downloads | non-blocking long-running work |
| `EPIC-008` Themes | theme integration and safe fallback |
| `EPIC-009` Localization | translated shell and locale-aware presentation |
| `EPIC-011` Game Library | systems, recent content and custom collections |
| `EPIC-022` Controller Management | controller semantics and player devices |
| `EPIC-023` Keyboard & Mouse | optional secondary input |
| `EPIC-043` Display / TV Mode | handheld/external-display behavior |
| `EPIC-045` Power & Suspend | actual Sleep/Restart/Power Off behavior |
| `EPIC-050` Crash Recovery / Safe Mode | safe shell recovery |
| `ENABLER-001` SYSTEM / STATE / USERDATA | persistent shell/user state |
| `ENABLER-002` Multi-device H700 Abstraction | board-specific display/input differences |
| `ENABLER-003` Boot / Lifecycle Reliability | deterministic shell startup and return |
| `ENABLER-004` Hardware Capability Baseline | graphics, input and display primitives |

### Platform requirements

EPIC-001 itself is primarily a userspace/product Epic. It requires the platform to expose reliable primitives rather than nuubUI-specific kernel functionality.

| Area | Required platform capability |
|---|---|
| Input | Stable internal and external controller event devices; predictable buttons/axes; simultaneous input; reconnect/resume behavior. |
| Graphics | DRM/KMS plus an accelerated rendering path suitable for the selected nuubUI stack. |
| Display | Supported internal panel modes and supported external-display modes must be discoverable and usable by the shell. |
| Process lifecycle | nuubUI must be able to launch, monitor and reap child/application processes and recover foreground control after their exit/crash. |
| IPC / services | Stable interface for notifications, background jobs, settings and system actions without shelling out to fragile UI scripts as a product architecture. |
| Power | Reliable system interfaces for suspend, reboot and poweroff. |
| Persistence | Writable persistent location for global shell state and per-user shell state outside the replaceable SYSTEM image. |
| Time | Correct system time for activity timestamps and notifications. |
| Optional input | Keyboard/mouse devices where supported by EPIC-023. |
| Multi-device | Device-specific orientation/resolution/input details must not require separate nuubUI products. |

### Current H700 support snapshot

This is a **known-state snapshot**, not the final Platform Capability Audit.

| Capability | Current state | Notes |
|---|---|---|
| Controller/internal input platform | `AVAILABLE / NEEDS FINAL MATRIX` | Core H700 input/controller bring-up exists; final nuubUI-oriented per-board validation still belongs to the capability audit. |
| External Bluetooth controller path | `AVAILABLE` | HID/UHID/HOG-class controller platform has already been validated during bring-up. |
| Local keyboard/console input | `AVAILABLE` | tty1 + external keyboard support already exists as an advanced/recovery facility. |
| Graphics/rendering primitives | `AVAILABLE / VERIFY FOR UI STACK` | Base platform graphics bring-up exists, but the future nuubUI rendering technology has not yet been selected or validated. |
| Integrated-display UX | `UNTESTED ON CURRENT RG35XX PRO DEV UNIT` | Its LCD is physically disconnected; validation must use RG CubeXX / RG40XX-V and/or a restored display target. |
| External-display/TV UX | `TO AUDIT` | EPIC-043 will define exact external-display behavior. |
| s2idle / wake | `AVAILABLE` | Production s2idle and deterministic wake are already validated at platform level. |
| reboot / shutdown primitives | `AVAILABLE / FINAL AUDIT REQUIRED` | Platform lifecycle capability exists; final product acceptance remains to be derived from ENABLER-003/EPIC-045. |
| Application isolation/session manager | `NOT IMPLEMENTED` | Product/userspace architecture still to be designed. |
| nuubUI shell | `NOT IMPLEMENTED` | Intentional: UI technology and implementation start only after product refinement. |
| Notification integration | `NOT IMPLEMENTED` | Covered by EPIC-006. |
| Background-job integration | `NOT IMPLEMENTED` | Covered by EPIC-007. |
| Safe shell recovery | `PARTIAL ENABLERS ONLY` | Local console exists, but product-level safe-mode/shell recovery is not yet implemented. |
| Home / navigation / collections UX | `NOT IMPLEMENTED` | Product work. |

### Gaps identified

No new kernel patch is justified directly by this refinement.

Known product/userspace gaps:

1. select the nuubUI technology/architecture;
2. implement the persistent shell/session manager;
3. implement the controller-navigation framework;
4. implement the Home information architecture;
5. define common application launch/exit/session contracts;
6. define IPC/service APIs used by Settings, Notifications, Background Jobs and Power;
7. implement context restoration;
8. implement responsive handheld/TV rendering after EPIC-043 is refined;
9. implement shell-level error presentation;
10. implement safe shell fallback/recovery with EPIC-050;
11. integrate custom collections supplied by EPIC-011;
12. validate the selected UI stack against actual H700 GPU/display/input capabilities.

### Decisions intentionally left open

The following are **not** decided by EPIC-001 refinement:

- nuubUI implementation language/framework/toolkit;
- exact visual design;
- final Home layout geometry;
- animation style;
- theme file format;
- exact service/IPC technology;
- compositor architecture, if any;
- exact external-display policy;
- exact user/session persistence format.

These decisions must be made only after the relevant product requirements and platform constraints are understood.

### Refinement result

EPIC-001 is considered **REFINED**.

The following are now product requirements:

- nuubUI is the primary persistent console shell;
- controller-only ordinary operation;
- content-first Home;
- first-class Games, Applications, Settings and Power areas;
- custom collections surfaced in Games / Collections;
- explicit Sleep / Restart / Power Off access;
- progressive disclosure of advanced functionality;
- application crash isolation;
- context-aware return to nuubUI where practical;
- one responsive shell for handheld and TV;
- offline-capable local core experience.

Implementation technology remains deliberately undecided.

---

## EPIC-002 — First Boot / Onboarding

**Status:** `REFINED`

### Product intent

The nuubOS first-boot experience must be **short, obvious and console-like**.

Its purpose is only to collect the minimum information required to make the device immediately usable. Optional online services and advanced configuration must not turn first boot into a long setup procedure.

Approved flow:

```text
Welcome
  ↓
Timezone / Date & Time
  ↓
Wi-Fi                    [Skip]
  ↓
Add Users
  ↓
Ready
  ↓
nuubUI
```

### Scope

EPIC-002 owns:

- first-boot detection;
- Welcome screen;
- timezone selection;
- automatic/manual date and time setup;
- basic Wi-Fi setup with a clear Skip path;
- creation of one or more local users;
- username selection;
- avatar selection from bundled nuubOS avatars;
- persistence of onboarding completion;
- safe continuation if onboarding is interrupted;
- transition from onboarding to the normal nuubUI experience.

### Out of scope

The following must **not** be required during onboarding:

- language selection;
- ScreenScraper configuration;
- RetroAchievements configuration;
- Syncthing configuration;
- Moonlight / Steam configuration;
- Plex/media-service configuration;
- controller remapping;
- advanced network configuration;
- SSH/SMB/SFTP/Web service configuration;
- theme selection;
- performance tuning;
- update-channel selection;
- any mandatory cloud account.

Those functions remain available later through their dedicated Settings / Epics.

### Approved onboarding behavior

#### 1. Welcome

A minimal introduction to nuubOS with one obvious action to begin setup.

#### 2. Timezone / Date & Time

The user selects the timezone.

Automatic time synchronization is enabled by default.

```text
Automatic time: ON
Source: NTP when network connectivity becomes available
```

Because Wi-Fi configuration occurs in the following step, onboarding must not block waiting for NTP.

If networking becomes available later in the wizard, time synchronization can occur automatically in the background.

The user must be able to disable automatic time and provide date/time manually when required.

#### 3. Wi-Fi

The user may:

- scan available networks;
- choose a network;
- enter its credentials;
- connect;
- continue after a successful connection;
- explicitly choose **Skip** and finish onboarding offline.

Internet connectivity is therefore **not mandatory** for completing first boot.

Advanced networking remains outside this Epic.

#### 4. Add Users

At least one local user must be created.

For each user:

- choose a username;
- choose a profile picture/avatar from the avatars bundled with nuubOS.

The onboarding flow may create multiple users:

```text
Create user
  ↓
Add another user?
  ├── Yes → Create user
  └── No  → Continue
```

Authentication, default-user behavior, user selection at boot and ownership/isolation of user data are defined by `EPIC-003 — Multi-user Profiles`.

#### 5. Ready

A final minimal completion screen confirms that initial setup is complete.

No additional service configuration is inserted before completion.

After confirmation, control passes to the normal nuubUI user/session flow.

### User Stories

| ID | Priority | User Story |
|---|---|---|
| `US-ONB-001` | MUST | As a new user, I want a simple Welcome screen so that I immediately understand that the console needs a short initial setup. |
| `US-ONB-002` | MUST | As a user, I want to select my timezone and use automatic network time so that the console maintains correct date and time without manual maintenance. |
| `US-ONB-003` | MUST | As a user, I want to manually set date and time when automatic synchronization is disabled or unavailable. |
| `US-ONB-004` | MUST | As a user, I want to connect to Wi-Fi during first boot so that online functionality can work immediately. |
| `US-ONB-005` | MUST | As a user, I want to skip Wi-Fi configuration so that lack of Internet access never prevents me from using the console locally. |
| `US-ONB-006` | MUST | As a user, I want to create my local profile by choosing a username and one of the available profile pictures. |
| `US-ONB-007` | MUST | As a household, we want to create multiple users during onboarding without having to complete first boot separately for each person. |
| `US-ONB-008` | MUST | As a user, I want onboarding to end immediately after the essential setup so that optional services can be configured later. |
| `US-ONB-009` | MUST | As a user, I want completed onboarding data to survive reboot/power loss so that I do not have to restart setup unnecessarily. |
| `US-ONB-010` | MUST | As a user, I want an interrupted onboarding flow to resume safely without corrupting already saved setup data. |

### Acceptance Criteria

| User Story | PASS condition |
|---|---|
| `US-ONB-001` | An uninitialized nuubOS installation enters the Welcome flow automatically and exposes an unambiguous controller-driven action to begin. |
| `US-ONB-002` | The user can select a valid timezone; automatic time is enabled by default; when network connectivity becomes available, NTP can synchronize the system clock without requiring a new onboarding step. |
| `US-ONB-003` | With automatic time disabled, the user can enter a valid local date/time and continue onboarding without network connectivity. |
| `US-ONB-004` | The Wi-Fi step can scan visible networks, accept credentials for a selected supported network and report connection success/failure without requiring access to advanced network settings. |
| `US-ONB-005` | Selecting Skip at the Wi-Fi step allows onboarding to continue and complete with no active network connection. Core local nuubUI operation remains available. |
| `US-ONB-006` | Onboarding cannot complete until at least one valid local user exists. Creating a user requires a valid username and selection of one bundled avatar. |
| `US-ONB-007` | After creating a user, the wizard offers a controller-driven path to add another user or continue. Multiple users created in the same onboarding session persist independently. |
| `US-ONB-008` | After the user step, the wizard proceeds directly to a Ready/completion state. No optional online service or advanced configuration is mandatory before entering normal nuubUI. |
| `US-ONB-009` | Once onboarding completion is committed, a normal reboot returns to the normal nuubUI/session flow rather than restarting first-boot setup. A factory-reset/recovery operation may intentionally clear this state. |
| `US-ONB-010` | Power loss/reboot during onboarding never produces a partially initialized state that prevents setup from continuing. Previously committed valid values may be preserved; incomplete values must be safely requested again. |

### Dependencies

| Dependency | Relationship |
|---|---|
| `EPIC-001` nuubUI | controller-driven visual/navigation framework and transition to the main shell |
| `EPIC-003` Multi-user Profiles | user identity, profile persistence, default-user/user-picker behavior |
| `EPIC-040` Wi-Fi Manager | network discovery, connection and stored Wi-Fi configuration |
| `EPIC-053` Date / Time / RTC | timezone, system clock and NTP behavior |
| `ENABLER-001` SYSTEM / STATE / USERDATA | persistent onboarding/user/network state |
| `ENABLER-003` Boot / Lifecycle Reliability | first-boot detection and reliable resume after reboot/power loss |
| `ENABLER-005` Security Baseline | safe handling of Wi-Fi credentials and user/profile state |

### Platform requirements

EPIC-002 is predominantly userspace/product logic.

| Area | Required platform capability |
|---|---|
| Input | Controller input must be available before and throughout onboarding. |
| Display | A usable native display path must be available during first boot. |
| Persistence | Writable persistent STATE must exist before onboarding data is committed. |
| RTC / time | System clock APIs, timezone database/support and NTP-capable networking must be available. |
| Wi-Fi | Scan/connect/disconnect primitives and secure persistence of network configuration. |
| Randomness/security | Suitable system randomness for secure networking/credential handling where required by dependent services. |
| Boot state | A deterministic way to distinguish `onboarding incomplete` from `onboarding complete`. |
| Recovery | Invalid/incomplete onboarding state must be recoverable without modifying the immutable SYSTEM image. |

### Current H700 support snapshot

This is a known-state snapshot, not the final capability audit.

| Capability | Current state | Notes |
|---|---|---|
| Controller input during boot/userspace | `AVAILABLE / FINAL UI VALIDATION REQUIRED` | Core input platform exists; onboarding integration is not implemented. |
| Persistent STATE architecture | `PARTIAL / TO REFINE` | Separation principles exist, but final multi-user/onboarding layout is not frozen yet. |
| Wi-Fi platform | `AVAILABLE` | RTL8821CS production Wi-Fi is already platform-final; user-facing Wi-Fi management remains to be built. |
| Offline boot/use | `AVAILABLE` | Network connectivity is not required by the base Linux platform. |
| RTC/time platform | `TO AUDIT` | Product requirements are now explicit; final RTC/timezone/NTP capability will be refined under EPIC-053. |
| User/profile store | `NOT IMPLEMENTED` | Defined by EPIC-003 and ENABLER-001. |
| First-boot state machine | `NOT IMPLEMENTED` | Product/userspace work. |
| Onboarding UI | `NOT IMPLEMENTED` | Product/userspace work. |
| Bundled avatar set | `NOT DEFINED` | Asset/theme/licensing decision still required. |

### Gaps identified

No new kernel change is justified directly by this Epic.

Known gaps:

1. define the persistent first-boot state and transactional completion marker;
2. define the user/profile persistent schema with EPIC-003;
3. implement the onboarding state machine;
4. implement timezone selection and automatic/manual clock flow;
5. integrate basic Wi-Fi discovery/connection from EPIC-040;
6. define the bundled avatar set and its licensing/provenance;
7. define safe persistence semantics for partially completed onboarding;
8. define the exact transition from `Ready` to the active-user/session behavior established by EPIC-003;
9. validate RTC/timezone/NTP behavior during the later platform audit.

### Decisions intentionally left open

EPIC-002 does not yet decide:

- exact Welcome artwork/copy;
- exact visual layout;
- number/style of bundled avatars;
- username character/length policy;
- avatar extensibility/custom user images after onboarding;
- active-user selection after Ready when multiple users exist;
- precise persistent storage format;
- whether timezone is presented as regions/cities, searchable list or another controller-friendly selector.

### Refinement result

EPIC-002 is considered **REFINED**.

The following product decisions are now fixed:

```text
Welcome
  ↓
Timezone / Date & Time
  ↓
Wi-Fi [Skip]
  ↓
Add Users [username + bundled avatar]
  ↓
Ready
```

Additional decisions:

- automatic NTP time is the default;
- lack of Wi-Fi never blocks onboarding;
- at least one local user is mandatory;
- multiple users may be created in the first-boot flow;
- optional online services are configured later;
- onboarding completion is persistent and interruption-safe.

---

## EPIC-003 — Multi-user Profiles

**Status:** `REFINED`

### Product intent

nuubOS supports multiple local user profiles while keeping the console experience lightweight and immediate.

A user profile represents a personal gaming/session context, not a desktop-style Unix login environment.

Each user must have isolated personal data and preferences while sharing device-level resources such as installed applications, ROMs, BIOS files and common metadata.

Only **one user session is active at a time**.

### Approved ownership model

#### Per-user data

The following are owned by the active user:

```text
Profile
├── username
├── avatar
│
├── UI
│   ├── selected theme
│   ├── UI language
│   ├── accessibility preferences
│   └── user-level UI preferences
│
├── Game Library
│   ├── favorites
│   ├── recent games
│   ├── custom collections
│   ├── play statistics
│   └── play history
│
├── Gaming
│   ├── saves
│   ├── save states
│   ├── screenshots / captures where configured as personal
│   ├── emulator configuration
│   ├── RetroArch configuration
│   ├── per-system emulator/core preferences
│   └── per-game overrides
│
├── Controllers
│   ├── mappings
│   ├── preferred controller
│   ├── player assignment
│   ├── controller order
│   └── controller-specific preferences
│
├── Online
│   ├── RetroAchievements account/configuration
│   ├── Syncthing configuration
│   └── other explicitly personal service accounts
│
└── Session state
    ├── last UI context
    ├── personal notification state where applicable
    └── other personal application state
```

#### Global device data

The following remain device-wide:

```text
Device
├── Wi-Fi networks
├── Bluetooth pairings
├── timezone / RTC
├── brightness
├── volume
├── physical audio route/state
├── display hardware configuration
├── storage configuration
├── power policy defaults
├── OTA / update channel
├── SSH / SCP / SFTP / SMB / Web service enablement
├── device service credentials
├── installed themes
├── installed emulator binaries
├── installed RetroArch cores
├── installed applications
├── ROMs
├── BIOS files
├── shared game metadata/artwork
└── hardware capabilities
```

### Shared software, personal configuration

Emulation follows this rule:

```text
GLOBAL / SHARED
├── RetroArch installation
├── installed libretro cores
├── standalone emulator binaries
└── BIOS availability

PER USER
├── RetroArch settings
├── emulator settings
├── preferred core/emulator per system
├── per-game overrides
├── shaders
├── aspect-ratio preferences
├── input mappings
└── save/state behavior
```

The console therefore owns the software; the user owns how that software behaves for their session.

### Bluetooth ownership model

Bluetooth pairing is **global to the device**.

A controller must not require re-pairing when switching users.

Controller behavior is user-specific:

```text
Xbox Controller #1
├── paired device-wide
│
├── User A
│   ├── Player 1
│   └── custom mapping
│
└── User B
    ├── Player 2
    └── standard mapping
```

The same principle applies to other supported controllers.

### Shared game content and metadata

The following are shared:

- ROMs;
- BIOS files;
- base game metadata;
- artwork;
- common downloadable game media.

Personal state is layered on top:

- favorite status;
- custom collection membership;
- playtime;
- last played;
- play history;
- save files;
- save states;
- user-specific emulator/controller settings.

ScreenScraper or equivalent metadata must therefore not duplicate identical shared assets per user unless technically necessary.

### Session model

nuubOS has exactly **one active user at a time**.

No simultaneous multi-session desktop model is required.

Conceptually:

```text
Device
└── Active session: User A
```

Switching user follows the logical sequence:

```text
active user
  ↓
flush pending personal state
  ↓
stop/suspend user-scoped services
  ↓
activate new profile
  ↓
load new user's configuration/state
  ↓
start new user's services
  ↓
return to nuubUI
```

The exact implementation mechanism is intentionally left open.

### Startup behavior

#### One configured user

The user-selection step is invisible:

```text
Boot
  ↓
single user
  ↓
Home
```

#### Multiple configured users

The device must support both:

```text
Boot behavior
├── Show user picker
└── Direct login
     └── Default user: <configured profile>
```

This is a device-wide setting.

### Switch User

A user must be able to switch profile **without rebooting the device**.

The profile/user area in nuubUI must expose a clear `Switch User` action whenever more than one selectable profile exists.

The outgoing user's state must be flushed before activating the next user.

### User creation

New users can be created:

- during First Boot / Onboarding;
- later from Settings.

A normal user has at least:

- username;
- avatar.

No password/PIN is required by default.

This Epic defines local console profiles, not hostile security boundaries between mutually untrusted users.

Optional profile-lock/PIN functionality is not part of the current MUST scope.

### User deletion

Deleting a user must be an explicit destructive action.

The UI must clearly distinguish personal data from shared console content.

Deleting a user may remove:

- user settings;
- statistics;
- favorites/recent/custom collections;
- save files;
- save states;
- personal emulator/controller configuration;
- RetroAchievements configuration;
- Syncthing configuration;
- other user-scoped state.

It must **not** remove:

- ROMs;
- BIOS files;
- installed applications;
- installed emulators/cores;
- shared artwork/metadata;
- device-wide network/service configuration.

Deletion must require explicit confirmation.

Backup/export behavior before deletion is handled by `EPIC-035 — Backup & Restore`.

### Guest profile

Guest Mode is a **SHOULD** capability.

Guest is a special temporary session intended for casual use without contaminating normal users.

By default Guest must not persist:

- RetroAchievements identity;
- Syncthing configuration;
- favorites;
- recent history;
- play statistics;
- custom collections;
- personal emulator/controller preferences beyond the active session.

Guest saves/states and other personal session data are **ephemeral by design** and are discarded when the Guest session ends.

A user who needs persistent progress should use or create a normal profile.

Guest uses shared ROMs, BIOS, applications and metadata like any other profile.

The exact UX for entering Guest Mode is left to nuubUI refinement, but Guest must be clearly distinguishable from normal persistent users.

### User Stories

| ID | Priority | User Story |
|---|---|---|
| `US-USR-001` | MUST | As a user, I want a personal local profile with my username and avatar. |
| `US-USR-002` | MUST | As a user, I want statistics, favorites, recent games and custom collections to be independent from other users. |
| `US-USR-003` | MUST | As a user, I want my save files and save states to be isolated from every other user. |
| `US-USR-004` | MUST | As a user, I want my own emulator and RetroArch configuration. |
| `US-USR-005` | MUST | As a user, I want my own controller mappings, preferred controllers, player order and assignments. |
| `US-USR-006` | MUST | As a user, I want my own RetroAchievements account/configuration. |
| `US-USR-007` | MUST | As a user, I want my own Syncthing configuration and synchronization scope. |
| `US-USR-008` | MUST | As a household, we want to share ROMs, BIOS, applications, emulators and common metadata without duplicating them per user. |
| `US-USR-009` | MUST | As the only configured user, I want the console to enter my profile directly without showing an unnecessary user picker. |
| `US-USR-010` | MUST | With multiple users, I want the device to support either a user picker at boot or direct login to a configured default profile. |
| `US-USR-011` | MUST | As a user, I want to switch profile without rebooting the console. |
| `US-USR-012` | MUST | As a user, I want my pending personal state to be safely saved before another profile becomes active. |
| `US-USR-013` | MUST | As a device owner, I want to add new users later from Settings. |
| `US-USR-014` | MUST | As a device owner, I want to delete a user without deleting shared ROMs, BIOS, applications or metadata. |
| `US-USR-015` | MUST | As a user, I want personal services never to use credentials/configuration belonging to another user. |
| `US-USR-016` | MUST | As a user, I want my selected theme and UI language to follow my profile. |
| `US-USR-017` | MUST | As a household, we want device-level brightness, volume, networking, storage and system maintenance settings to remain shared rather than changing user ownership. |
| `US-USR-018` | SHOULD | As a guest, I want a temporary profile that lets me use the console without modifying persistent users' personal state. |

### Acceptance Criteria

| User Story | PASS condition |
|---|---|
| `US-USR-001` | A persistent user profile can be created with a valid username and avatar and is restored after reboot. |
| `US-USR-002` | Changes to favorites, recent activity, play statistics or custom collections made by User A are not visible as User B's personal state. |
| `US-USR-003` | Launching the same game as two different users resolves to distinct save and save-state namespaces; one user's save/state writes cannot overwrite another user's data. |
| `US-USR-004` | A user can change supported emulator/RetroArch preferences without altering another user's configuration for the same system/game. |
| `US-USR-005` | Controller mapping/order/player assignment can differ between two users while the underlying physical controller pairing remains device-global. |
| `US-USR-006` | RetroAchievements credentials/session/configuration for one user are never automatically used when another user becomes active. |
| `US-USR-007` | Syncthing configuration and synchronized user-scoped content are logically isolated between users. The implementation may use one or multiple daemon instances, but cross-user synchronization must not occur unintentionally. |
| `US-USR-008` | Two users can launch the same shared ROM and use the same installed emulator/core/BIOS metadata without duplicate content copies being required per profile. |
| `US-USR-009` | With exactly one persistent user and no explicit override requiring selection, normal boot reaches that user's nuubUI session without a user-picker interaction. |
| `US-USR-010` | With multiple users, Settings can select `Show user picker` or `Direct login`; Direct login requires choosing a valid default persistent user. |
| `US-USR-011` | Selecting `Switch User` transitions to another profile without rebooting the Linux system and returns to a responsive nuubUI session. |
| `US-USR-012` | Before profile activation changes, dirty user-scoped configuration/state is flushed or transactionally committed so a normal switch cannot silently lose completed user actions. |
| `US-USR-013` | A new persistent user can be created from Settings using controller-only interaction and becomes selectable without factory reset/re-onboarding. |
| `US-USR-014` | User deletion requires explicit confirmation; after deletion that user's personal namespace is removed according to policy while shared ROM/BIOS/application/metadata content remains intact. |
| `US-USR-015` | Switching users changes the active namespace for personal credentials/configuration before starting user-scoped services. No personal service may continue under the previous user's identity after a completed switch. |
| `US-USR-016` | Two users may select different installed themes and UI languages; switching users applies the target user's selections without changing the other profile. |
| `US-USR-017` | Changing brightness, volume, Wi-Fi/device network configuration, storage configuration or system update settings affects the device globally and does not create conflicting per-user copies. |
| `US-USR-018` | Guest Mode can be entered without creating a persistent normal profile; it uses shared content but does not modify persistent users' personal state. Ending the Guest session removes Guest personal/session data according to the Guest ephemeral policy. |

### Dependencies

| Dependency | Relationship |
|---|---|
| `EPIC-001` nuubUI | profile presentation, active-user UI, Switch User flow |
| `EPIC-002` First Boot / Onboarding | initial creation of one or more users |
| `EPIC-004` Settings | creation/deletion/default-login and user preferences |
| `EPIC-008` Themes | installed shared themes and per-user theme selection |
| `EPIC-009` Localization | per-user UI language |
| `EPIC-011` Game Library | favorites, recents, collections and personal metadata state |
| `EPIC-013` RetroArch Integration | per-user RetroArch configuration |
| `EPIC-017` RetroAchievements | per-user online identity |
| `EPIC-019` Saves & Save States | per-user save/state namespaces |
| `EPIC-020` Play Statistics | per-user statistics/history |
| `EPIC-022` Controller Management | per-user mapping/order/assignment |
| `EPIC-034` Syncthing | per-user synchronization configuration |
| `EPIC-035` Backup & Restore | backup/export of user state |
| `ENABLER-001` SYSTEM / STATE / USERDATA | persistent ownership model and namespaces |
| `ENABLER-005` Security Baseline | permissions/credential isolation |

### Platform / architecture requirements

EPIC-003 is primarily a userspace/data-architecture Epic.

| Area | Required capability |
|---|---|
| Persistent storage | Reliable per-user namespaces outside immutable SYSTEM. |
| Shared storage | Shared read/write content namespaces for ROMs, BIOS, metadata and installed assets as appropriate. |
| Session manager | Exactly one active persistent/Guest user context at a time. |
| Service lifecycle | User-scoped services must stop/reload/restart against the target user context during profile switching. |
| Process environment | Launched applications/emulators must receive the active user's configuration/save/state paths deterministically. |
| Permissions | Personal configuration/credentials must not be accidentally shared through common writable paths. |
| Atomic state | Profile-switch and user-deletion operations must avoid partially applied ownership/state transitions. |
| Controller layer | Device-global physical pairing plus user-specific mapping/assignment. |
| UI/localization | User-specific theme/language can be applied after session activation. |
| Guest | Temporary writable session namespace that can be discarded cleanly. |

### Current H700 support snapshot

This is a known-state snapshot, not the final capability audit.

| Capability | Current state | Notes |
|---|---|---|
| Shared Linux platform/content capability | `AVAILABLE` | No platform limitation prevents shared ROM/BIOS/application storage. |
| Persistent STATE/USERDATA concept | `PARTIAL / NEEDS FINAL SCHEMA` | Architectural separation exists but per-user ownership/layout is not yet finalized. |
| Device-global Bluetooth pairing | `AVAILABLE` | BlueZ/platform pairing persistence already fits the approved ownership model conceptually. |
| Controller user mappings | `NOT IMPLEMENTED` | Product/userspace layer required. |
| User profile database/store | `NOT IMPLEMENTED` | Must be designed. |
| Active-user session manager | `NOT IMPLEMENTED` | Must be designed. |
| Per-user save/state routing | `NOT IMPLEMENTED` | Defined later with EPIC-019. |
| Per-user emulator config | `NOT IMPLEMENTED` | Defined with emulator Epics. |
| Per-user RetroAchievements | `NOT IMPLEMENTED` | Defined with EPIC-017. |
| Per-user Syncthing | `NOT IMPLEMENTED` | Defined with EPIC-034. |
| Guest temporary namespace | `NOT IMPLEMENTED` | Userspace/data architecture work. |
| Theme/language per-user | `NOT IMPLEMENTED` | Depends on theme/localization implementation. |
| Global volume/brightness | `PLATFORM CAPABILITY EXISTS / PRODUCT INTEGRATION TBD` | Physical device-state semantics are approved as global. |

### Gaps identified

No kernel change is currently justified by this Epic.

Known architecture/userspace gaps:

1. define the persistent user/profile schema;
2. define final directory/database ownership under STATE and USERDATA;
3. implement active-user session management;
4. implement deterministic per-user path/environment injection for launched applications;
5. implement profile switching with transactional flush;
6. define user-scoped service lifecycle;
7. implement device-global/default-login settings;
8. implement per-user emulator/controller configuration routing;
9. implement per-user RetroAchievements identity;
10. implement per-user Syncthing scope;
11. implement safe user deletion;
12. implement Guest ephemeral namespace and cleanup;
13. define migration/versioning of profile schema across OTA updates;
14. define backup/export semantics with EPIC-035.

### Decisions intentionally left open

The following remain open:

- exact filesystem paths;
- database versus filesystem representation of profile metadata;
- exact username character/length rules;
- custom avatar support after onboarding;
- optional profile PIN/lock in a future iteration;
- exact Guest entry/exit UI;
- Syncthing daemon topology;
- exact user-scoped service IPC/session mechanism;
- whether screenshots are always personal or can optionally be stored in a shared gallery;
- fine-grained ownership of future application-specific accounts not yet defined.

### Refinement result

EPIC-003 is considered **REFINED**.

Approved product decisions include:

- exactly one active user session at a time;
- one user → direct boot with no picker;
- multiple users → configurable picker or direct login to a default user;
- Switch User without device reboot;
- emulator configuration is per-user;
- controller mapping/order/assignment is per-user;
- Bluetooth physical pairing remains device-global;
- saves and save states are strongly isolated per-user;
- RetroAchievements is per-user;
- Syncthing is per-user;
- favorites, recents, collections and statistics are per-user;
- theme and UI language are per-user;
- brightness and volume are device-global;
- ROMs, BIOS, installed software and common metadata are shared;
- Guest Mode is supported as a SHOULD capability with ephemeral personal state.

---

## EPIC-004 — Settings

**Status:** `REFINED`

### Product intent

nuubOS Settings must provide a **simple, controller-first, console-style** way to configure the device and the active user.

The normal Settings experience must expose clear product concepts rather than Linux implementation details.

Common options must be immediately understandable. Advanced functionality remains available, but only inside the relevant functional area and outside the normal path.

Settings is a shared UI framework: each dedicated Product Epic owns the meaning and behavior of its options, while EPIC-004 owns how those options are presented, edited, validated, persisted and recovered safely.

### Core principles

1. **Console-style, not desktop-style**
   Settings must feel like a console settings menu rather than a desktop control panel.

2. **Controller first**
   Every ordinary setting must be fully usable with a controller.

3. **Clear ownership**
   The UI must make it understandable whether a setting belongs to:
   - the active user; or
   - the whole device.

4. **Progressive disclosure**
   Frequently used settings remain visible. Advanced settings are available inside the relevant category without cluttering the default view.

5. **Product terminology**
   User-facing labels describe the product behavior, not kernel modules, sysfs nodes, daemon names or Linux implementation details.

6. **Safe application model**
   Reversible low-risk settings may apply immediately. Risky or disruptive changes require confirmation, preview/rollback or another safe transaction model.

7. **No unnecessary global Apply button**
   Settings should persist as soon as a change is safely accepted. The product must not behave like a desktop preference dialog requiring users to remember to press a global `Apply`.

8. **Atomic persistence**
   A completed setting change must not leave corrupt or partially written configuration after reboot or unexpected power loss.

9. **Hardware-aware presentation**
   Options that are not supported by the current device should normally not be exposed. Temporarily unavailable supported options should remain understandable and show why they cannot currently be changed.

10. **Recoverability**
    Invalid settings must not permanently prevent normal nuubUI startup or access to recovery.

### Approved top-level information architecture

The exact visual layout is intentionally left open, but the functional hierarchy is:

```text
Settings
├── Profile
│   ├── username / avatar
│   ├── language
│   ├── theme
│   ├── accessibility
│   └── other per-user UI preferences
│
├── Gaming
│   ├── emulator defaults
│   ├── save/state behavior
│   ├── RetroAchievements
│   ├── performance preferences
│   └── Advanced
│
├── Library
│   ├── ROM/library locations
│   ├── scanning
│   ├── metadata / ScreenScraper
│   └── Advanced
│
├── Controllers
│   ├── mappings
│   ├── player assignment
│   ├── controller order
│   └── Advanced
│
├── Display
│   ├── brightness
│   ├── external display / TV
│   ├── visual options
│   └── Advanced
│
├── Audio
│   ├── volume / mute
│   ├── output route
│   ├── Bluetooth audio
│   └── Advanced
│
├── Wi-Fi
│   ├── networks
│   ├── connection
│   └── Advanced
│
├── Bluetooth
│   ├── paired devices
│   ├── pairing / removal
│   └── Advanced
│
├── Storage
│   ├── available storage
│   ├── content locations
│   ├── removable media
│   └── Advanced
│
├── Users
│   ├── add / delete user
│   ├── boot user behavior
│   └── default user
│
├── Services & Sharing
│   ├── SSH / SCP / SFTP
│   ├── SMB
│   ├── Web administration
│   ├── Syncthing
│   └── service credentials
│
├── Notifications
│   └── notification preferences
│
├── System
│   ├── date / time
│   ├── power behavior
│   ├── system information
│   ├── diagnostics
│   └── Advanced / Developer
│
└── Updates
    ├── current version
    ├── update availability
    ├── update channel if supported
    └── update actions
```

This hierarchy is a **product taxonomy**, not a commitment to a particular number of screens.

The dedicated Power menu from EPIC-001 remains outside Settings for immediate actions:

```text
Sleep
Restart
Power Off
```

Settings may contain **power behavior/preferences**, but not hide the primary power actions.

### User versus device settings

Settings must respect the ownership model approved by EPIC-003.

#### Per-user examples

- language;
- theme;
- accessibility preferences;
- emulator configuration;
- RetroArch settings;
- per-system/per-game gaming preferences;
- controller mappings/order/assignment;
- RetroAchievements;
- Syncthing user configuration;
- personal library behavior where applicable.

#### Device-wide examples

- Wi-Fi;
- Bluetooth pairings;
- timezone/RTC;
- brightness;
- volume;
- physical audio route;
- storage;
- service enablement;
- OTA/update configuration;
- device credentials;
- system-level power policy.

The UI must avoid creating apparently per-user copies of settings that are actually device-global.

### Advanced settings model

There is **no required top-level generic `Advanced Settings` dump**.

Advanced options belong inside their relevant domain:

```text
Display
└── Advanced

Wi-Fi
└── Advanced

Gaming
└── Advanced
```

System-level developer/diagnostic functionality may exist under:

```text
System
└── Advanced / Developer
```

but must remain outside the normal user path.

Advanced settings may expose more technical terminology when necessary, but ordinary settings must remain product-oriented.

### Setting application semantics

Settings are classified by risk.

#### Immediate / reversible

Examples:

- brightness;
- volume;
- many UI preferences;
- theme preview/selection where safe.

These may apply immediately after selection.

#### Confirmed

Examples:

- enabling a network-facing remote service;
- deleting stored configuration;
- disconnecting/removing a device where data/state could be lost.

These require an explicit confirmation step.

#### Preview + automatic rollback

Used where a valid setting could make the device temporarily unusable.

Primary example:

```text
External display mode / resolution / refresh
```

A new mode may be previewed and automatically reverted if the user does not confirm within a defined safe period.

#### Destructive / recovery-class

Examples:

- user deletion;
- factory reset;
- destructive storage actions.

These belong to their dedicated Epic and must use stronger confirmation semantics.

### Setting availability rules

A setting can be:

```text
AVAILABLE
TEMPORARILY UNAVAILABLE
UNSUPPORTED ON THIS DEVICE
ADVANCED
```

Presentation rules:

- `AVAILABLE` → normally shown and editable;
- `TEMPORARILY UNAVAILABLE` → may be shown disabled with a concise reason;
- `UNSUPPORTED ON THIS DEVICE` → normally hidden from ordinary Settings;
- `ADVANCED` → available only after intentionally entering the relevant Advanced area.

This avoids presenting meaningless settings simply because another H700 board supports them.

### Help and descriptions

Settings with potentially unclear consequences must provide concise controller-friendly explanatory text.

Normal users must not need external documentation to understand what a common setting does.

Technical details may link or drill down into Advanced/Diagnostics where appropriate.

### Defaults and reset behavior

Every configurable setting must have a defined default.

Where useful, users should be able to restore:

- an individual setting;
- a logical settings section;
- user preferences;

without performing a full factory reset.

Resetting settings must not implicitly delete ROMs, BIOS, saves or unrelated user data.

Full system/user destructive recovery remains under EPIC-049.

### Search

Settings search is a **SHOULD** capability.

If implemented, search must:

- search user-facing labels/descriptions;
- return only settings applicable to the current device/session;
- preserve the same ownership and Advanced visibility rules;
- navigate to the actual setting rather than creating a separate configuration path.

Search is not required for first product bring-up if the category hierarchy remains small and clear.

### User Stories

| ID | Priority | User Story |
|---|---|---|
| `US-SET-001` | MUST | As a user, I want Settings organized into clear product categories so that I can find common options without understanding Linux. |
| `US-SET-002` | MUST | As a user, I want every ordinary setting to be usable entirely with a controller. |
| `US-SET-003` | MUST | As a user, I want to understand whether a setting applies to me personally or to the whole device. |
| `US-SET-004` | MUST | As a normal user, I want advanced options hidden from the primary path but still available inside the relevant settings category. |
| `US-SET-005` | MUST | As a user, I want safe settings to take effect without an unnecessary global Apply workflow. |
| `US-SET-006` | MUST | As a user, I want risky/disruptive settings to require confirmation or provide automatic rollback so that I cannot accidentally leave the console unusable. |
| `US-SET-007` | MUST | As a user, I want my accepted settings to survive reboot and unexpected power loss without corruption. |
| `US-SET-008` | MUST | As a user, I want unsupported settings to stay out of the way and temporarily unavailable settings to explain why they cannot currently be changed. |
| `US-SET-009` | MUST | As a user, I want common settings described using clear product language rather than Linux implementation terminology. |
| `US-SET-010` | MUST | As a user, I want sensible defaults so that nuubOS works without requiring manual tuning. |
| `US-SET-011` | MUST | As a user, I want to restore supported settings to defaults without deleting unrelated content or performing a factory reset. |
| `US-SET-012` | MUST | As a user, I want invalid configuration to be recoverable without permanently preventing nuubUI from starting. |
| `US-SET-013` | SHOULD | As a user, I want to search Settings when I do not know which category contains an option. |

### Acceptance Criteria

| User Story | PASS condition |
|---|---|
| `US-SET-001` | Every ordinary setting exposed by frozen Product Epics maps to a clear category in the approved Settings taxonomy and no normal flow requires navigating raw Linux configuration surfaces. |
| `US-SET-002` | Every ordinary Settings screen can be entered, changed, confirmed/cancelled and exited using controller input only. |
| `US-SET-003` | User-scoped and device-scoped settings are represented consistently enough that changing user does not create ambiguity over which values follow the profile and which remain global. |
| `US-SET-004` | Advanced options are absent from the normal category path until the user deliberately enters the relevant Advanced area; hiding them does not remove the underlying capability. |
| `US-SET-005` | Reversible low-risk settings persist after the user accepts/selects them without requiring a separate global Apply action. |
| `US-SET-006` | Every setting classified as risky/disruptive implements its declared safety mechanism. Display modes capable of making output unusable automatically revert unless explicitly confirmed. |
| `US-SET-007` | After a completed setting change, reboot/power loss cannot leave a syntactically corrupted or half-written setting value. Last committed valid state is recoverable. |
| `US-SET-008` | Device-unsupported ordinary options are not presented as usable controls. Temporarily unavailable supported options expose a reason rather than silently failing. |
| `US-SET-009` | Ordinary labels/descriptions do not require the user to understand kernel modules, sysfs paths, daemon/service implementation names or raw configuration files. |
| `US-SET-010` | A factory/default configuration can reach normal usable nuubUI operation without mandatory manual tuning of ordinary settings. |
| `US-SET-011` | Supported reset-to-default actions affect only their documented scope and do not delete ROMs, BIOS, saves or unrelated profile/device data. |
| `US-SET-012` | A documented safe configuration/recovery path can restore valid Settings state when user configuration prevents normal shell operation. |
| `US-SET-013` | If Settings search is implemented, a query can locate applicable user-facing settings and navigate to their canonical category without exposing hidden unsupported items. |

### Dependencies

| Dependency | Relationship |
|---|---|
| `EPIC-001` nuubUI | common navigation, focus, controller interaction and visual shell |
| `EPIC-003` Multi-user Profiles | ownership of user versus device settings |
| `EPIC-006` Notification System | feedback for asynchronous or completed configuration actions |
| `EPIC-009` Localization | localized labels/descriptions and per-user language |
| `EPIC-017` RetroAchievements | account/configuration surface |
| `EPIC-022` Controller Management | controller configuration |
| `EPIC-032` Storage Manager | storage configuration |
| `EPIC-034` Syncthing | synchronization configuration |
| `EPIC-037` Remote Services | service enablement |
| `EPIC-039` Device Credentials & Service Security | service credential management |
| `EPIC-040` Wi-Fi Manager | Wi-Fi settings |
| `EPIC-041` Bluetooth Manager | Bluetooth settings |
| `EPIC-043` Display / TV Mode | display settings and safe mode changes |
| `EPIC-044` Audio Management | audio settings |
| `EPIC-045` Power & Suspend | power behavior settings |
| `EPIC-048` OTA Updates | update configuration/actions |
| `EPIC-049` Recovery / Factory Reset | destructive reset/recovery operations |
| `EPIC-050` Crash Recovery / Safe Mode | recovery from bad UI/settings state |
| `EPIC-053` Date / Time / RTC | date/time settings |
| `EPIC-055` Privacy & Online Services | online-service visibility/control |
| `ENABLER-001` SYSTEM / STATE / USERDATA | persistent settings ownership/storage |
| `ENABLER-005` Security Baseline | permissions, credential handling and service exposure |

### Platform / architecture requirements

EPIC-004 is predominantly userspace architecture.

| Area | Required capability |
|---|---|
| Settings service/model | Typed settings with explicit key/schema, default, ownership, validation and availability semantics. |
| Persistence | Atomic writes to persistent STATE/user namespaces outside immutable SYSTEM. |
| Migration | Settings schema/version migration across OTA releases. |
| Ownership | Deterministic distinction between device-global and active-user values. |
| Validation | Invalid values rejected before becoming active/committed. |
| Change notification | Applications/services can observe relevant committed settings changes without polling fragile files. |
| Rollback | Risky settings can preserve and restore the last known-good value. |
| Hardware capability discovery | Settings UI can determine which options are supported/applicable on the current board. |
| Security | Secrets/credentials use protected storage/permissions and are not exposed as ordinary plaintext UI state. |
| Recovery | Last-known-good/default configuration can be activated without replacing SYSTEM. |

### Current H700 support snapshot

This is a known-state snapshot, not the final capability audit.

| Capability | Current state | Notes |
|---|---|---|
| Kernel/sysfs configuration primitives | `AVAILABLE FOR MANY AREAS` | Existing H700 bring-up already exposes many device controls, but raw interfaces are not the product Settings API. |
| CPU/GPU power modes | `AVAILABLE` | Platform power profiles already exist and can later be surfaced through product Settings. |
| Wi-Fi platform | `AVAILABLE` | Product Wi-Fi Manager/UI still pending. |
| Bluetooth platform | `AVAILABLE` | Product Bluetooth Manager/UI still pending. |
| s2idle/power primitives | `AVAILABLE` | Power behavior UI is not implemented. |
| Local keyboard/loadkeys advanced facility | `AVAILABLE` | Useful for recovery/advanced use, not ordinary Settings. |
| Settings schema/service | `NOT IMPLEMENTED` | Core userspace architecture still to be designed. |
| Atomic settings store | `NOT IMPLEMENTED AS PRODUCT LAYER` | ENABLER-001 must define final persistence model. |
| User/device ownership routing | `NOT IMPLEMENTED` | Defined conceptually by EPIC-003. |
| Hardware capability registry | `NOT IMPLEMENTED AS PRODUCT API` | Board abstraction must expose product-relevant capabilities. |
| Settings UI | `NOT IMPLEMENTED` | Depends on nuubUI. |
| Safe display rollback | `NOT IMPLEMENTED` | Defined later with EPIC-043. |
| Settings recovery/default restore | `NOT IMPLEMENTED` | Depends on recovery architecture. |

### Gaps identified

No kernel change is justified directly by this Epic.

Known architecture/userspace gaps:

1. define a typed/versioned settings schema;
2. define device-global versus per-user settings storage;
3. implement atomic settings persistence;
4. implement settings migration across OTA versions;
5. implement a stable change-notification/API layer for consumers;
6. implement hardware-capability-driven visibility;
7. implement Advanced visibility semantics;
8. implement safe confirmation/rollback primitives for disruptive settings;
9. implement scoped reset-to-default operations;
10. implement Settings UI in nuubUI;
11. integrate Settings recovery with EPIC-050/EPIC-049;
12. ensure secret settings are stored and displayed safely;
13. optionally implement settings search after the baseline taxonomy is stable.

### Decisions intentionally left open

The following remain open:

- exact settings storage format;
- database versus structured files;
- exact IPC/API technology;
- exact UI widget/control library;
- exact timeout for display-mode rollback;
- whether Settings search ships in the first public release;
- exact placement of application-specific settings that may emerge from later Epics;
- exact Developer-options unlock interaction;
- whether some Advanced options require an additional warning or explicit enable switch.

### Refinement result

EPIC-004 is considered **REFINED**.

Approved product decisions include:

- simple console-style category hierarchy;
- controller-only ordinary operation;
- explicit user/device ownership semantics;
- Advanced options live inside the relevant category;
- no generic desktop-style global Apply workflow;
- safe immediate application for reversible settings;
- confirmation/rollback for risky settings;
- atomic persistent settings;
- hardware-aware option visibility;
- product-oriented language;
- defined defaults and scoped reset-to-default support;
- recoverability from invalid configuration;
- Settings search is a SHOULD capability.

---

## EPIC-005 — Quick Menu / In-game Overlay

**Status:** `REFINED`

### Product intent

nuubOS must provide a fast, controller-driven **Quick Menu / in-game overlay** that exposes the most useful runtime actions without forcing the user to leave the current game or application.

The overlay is part of the nuubUI product experience, not a replacement for emulator/application advanced menus.

It must be:

- fast to open;
- controller-first;
- contextual;
- safe;
- visually lightweight;
- consistent across supported applications where possible;
- capable of exposing application-specific runtime actions without duplicating full Settings.

### Core interaction model

The Quick Menu is invoked through a dedicated controller action or shortcut defined by the controller/input architecture.

Opening the Quick Menu pauses or suspends the current experience only when that behavior is appropriate and supported.

Closing the Quick Menu returns directly to the running game/application.

Conceptually:

```text
Game / Application
  ↓
Quick Menu
  ├── Resume
  ├── Save / Load
  ├── Game / Application
  ├── Controller
  ├── Performance
  ├── Display / Audio
  └── Advanced / Contextual
  ↓
Resume
```

The exact visual layout and shortcut are intentionally left open.

### Approved functional hierarchy

```text
Quick Menu
├── Resume
│
├── Save / Load
│   ├── Save State
│   ├── Load State
│   └── State Slot
│
├── Game / Application
│   ├── Restart
│   ├── Screenshot
│   └── Quit to nuubUI
│
├── Controller
│   ├── player assignment
│   ├── mapping
│   └── rumble
│
├── Performance
│   ├── Performance Profile
│   │   ├── Auto
│   │   ├── Performance
│   │   └── Battery Saver
│   │
│   └── Statistics Overlay
│       ├── Off
│       ├── Basic
│       └── Advanced
│
├── Display / Audio
│   ├── Brightness
│   └── Volume
│
└── Advanced / Contextual
    └── application- or emulator-specific runtime options
```

Only capabilities that make sense for the active application are shown.

For example:

- `Save State` is shown only for applications/emulators that support save states;
- Moonlight may expose streaming statistics rather than emulator statistics;
- media applications may omit gaming-specific actions;
- unsupported controls are hidden rather than shown as meaningless disabled items, unless temporary unavailability needs explanation.

### Resume

`Resume` returns immediately to the running game/application.

It is always the safest/default Quick Menu action.

### Save / Load

For emulation environments that support save states, the Quick Menu must expose:

- Save State;
- Load State;
- active save-state slot;
- slot selection.

The exact save/state implementation belongs to EPIC-019.

Normal save files are not manually triggered here unless a specific emulator/application requires it.

### Game / Application actions

#### Restart

Restart relaunches or resets the active game/application using the currently active user/session configuration.

Restart must require deliberate selection but does not necessarily require a second confirmation unless data loss is possible.

#### Screenshot

Screenshot captures the current application/game output where supported.

Storage/ownership semantics are defined by EPIC-021 and EPIC-003.

#### Quit to nuubUI

Quit cleanly terminates the current game/application and returns to the previous nuubUI context where possible.

If unsaved volatile data could be lost, the application integration may require an appropriate confirmation.

### Controller runtime controls

The Quick Menu provides rapid access to controller-related runtime functions where applicable:

- current controller/player assignment;
- remapping;
- controller order;
- rumble level/enablement.

Persistent ownership remains per-user as defined by EPIC-003 and EPIC-022.

Changes may be:

```text
Session only
```

or, where explicitly supported:

```text
Save as user/game default
```

The exact persistence choice must be visible and unambiguous.

### Performance Profile

The Quick Menu exposes the currently active performance profile.

Initial supported product profiles are:

```text
Auto
Performance
Battery Saver
```

The underlying platform capability already exists, but runtime UX and persistence are handled by EPIC-047.

Where supported, changing profile from the Quick Menu should allow:

- temporary change for the current session;
- saving the selected profile as the active user's default for the current game/application.

The UI must clearly distinguish temporary runtime override from persistent per-game preference.

### Statistics Overlay

The Quick Menu provides an optional runtime statistics overlay.

Default state:

```text
Statistics Overlay: OFF
```

The statistics overlay must never be enabled automatically for normal users.

#### Basic mode

Basic mode is intended to be readable and useful without exposing excessive diagnostic data.

Candidate baseline:

```text
FPS
frame time
active performance profile
battery %
```

Only metrics that are reliable and meaningful on the active target are shown.

#### Advanced mode

Advanced mode is intended for power users, diagnostics and development.

Candidate metrics:

```text
FPS
frame time
CPU frequency
CPU utilization
GPU frequency
GPU utilization, where reliable
CPU/GPU temperature
RAM used / free
active performance profile
thermal throttling state
battery %
charging/discharging state
```

Optional metrics may be added where the platform exposes reliable data.

#### Application-specific statistics

Applications may add contextual statistics.

For emulation:

```text
FPS
frame time
CPU/GPU information
temperature
```

For game streaming:

```text
stream FPS
dropped frames
decode latency
network latency
bitrate
codec
```

For video/media playback:

```text
decode FPS
dropped frames
codec
hardware/software decode state
```

Context-specific metrics must be provided by the relevant application integration or platform service.

The Quick Menu must not fabricate or estimate metrics that cannot be measured reliably.

### Display / Audio runtime controls

The Quick Menu provides immediate access to safe physical runtime controls:

- brightness;
- volume.

These are device-global settings as defined by EPIC-003.

Changes apply immediately.

More disruptive display/audio routing settings remain in their dedicated Settings areas unless explicitly safe for Quick Menu exposure.

### Advanced / Contextual runtime options

Applications or emulator integrations may register runtime-specific actions.

Examples:

- aspect-ratio override;
- shader toggle;
- emulator-specific latency option;
- streaming bitrate profile;
- media subtitle selection.

The Quick Menu must not become a complete mirror of every application's advanced settings.

Only options valuable during an active session belong here.

### Context-sensitive presentation

Every Quick Menu item has capability/availability semantics.

Possible state:

```text
AVAILABLE
TEMPORARILY UNAVAILABLE
NOT APPLICABLE
ADVANCED
```

Rules:

- `AVAILABLE` → shown and usable;
- `TEMPORARILY UNAVAILABLE` → may be shown disabled with concise reason;
- `NOT APPLICABLE` → hidden;
- `ADVANCED` → placed behind the Advanced/Contextual area.

### Safety rules

The overlay must avoid accidental destructive actions.

Examples:

- `Resume` executes immediately;
- brightness/volume/profile changes may apply immediately;
- `Quit` must be deliberate;
- `Load State` should protect against accidental overwrite/loss according to EPIC-019 policy;
- destructive or irreversible application actions require explicit confirmation.

### User Stories

| ID | Priority | User Story |
|---|---|---|
| `US-QM-001` | MUST | As a user, I want to open a Quick Menu while using a supported game/application so that I can access common runtime actions without leaving the session. |
| `US-QM-002` | MUST | As a user, I want to resume the current session immediately from the Quick Menu. |
| `US-QM-003` | MUST | As an emulator user, I want to save and load save states and choose the active state slot from the Quick Menu when supported. |
| `US-QM-004` | MUST | As a user, I want to restart the active game/application without navigating back through the full library. |
| `US-QM-005` | MUST | As a user, I want to quit the active game/application and return predictably to nuubUI. |
| `US-QM-006` | MUST | As a user, I want to take a screenshot from the Quick Menu when capture is supported. |
| `US-QM-007` | MUST | As a user, I want quick access to relevant controller assignment/mapping/rumble controls during gameplay. |
| `US-QM-008` | MUST | As a user, I want to view and change the active performance profile while a game/application is running. |
| `US-QM-009` | MUST | As a user, I want to enable an FPS/performance statistics overlay when I need runtime performance information. |
| `US-QM-010` | MUST | As a normal user, I want the statistics overlay disabled by default so that gameplay remains clean. |
| `US-QM-011` | MUST | As a power user, I want an Advanced statistics mode exposing reliable CPU/GPU/thermal/memory information. |
| `US-QM-012` | MUST | As a streaming/media user, I want contextual statistics such as latency, bitrate, dropped frames or decode state when the active application can provide them. |
| `US-QM-013` | MUST | As a user, I want to change brightness and volume quickly without leaving the current session. |
| `US-QM-014` | MUST | As a user, I want the Quick Menu to show only actions relevant to the active application. |
| `US-QM-015` | MUST | As a user, I want temporary runtime changes and persistent per-game/user settings to be clearly distinguished. |
| `US-QM-016` | SHOULD | As a user, I want context-specific advanced runtime options to be exposed when they are genuinely useful during the active session. |

### Acceptance Criteria

| User Story | PASS condition |
|---|---|
| `US-QM-001` | A supported running application can invoke and close the Quick Menu using controller input without terminating the application. |
| `US-QM-002` | Selecting Resume closes the overlay and returns controller focus to the active session without an application restart. |
| `US-QM-003` | For integrations declaring save-state support, Save State, Load State and state-slot selection are controller-accessible and operate on the active user's save-state namespace. Unsupported applications do not expose these actions. |
| `US-QM-004` | Restart cleanly resets/relaunches the active game/application using the same active user and resolved runtime configuration. |
| `US-QM-005` | Quit terminates the active application and restores a responsive nuubUI context without exposing a terminal/blank dead state. |
| `US-QM-006` | On supported applications, Screenshot creates a valid capture associated with the active user/storage policy and reports success/failure. |
| `US-QM-007` | Runtime controller controls modify the active session as declared; any persistent change is explicitly saved to the active user's configuration rather than silently becoming global. |
| `US-QM-008` | The active performance profile is visible. Switching among supported profiles takes effect without application restart where the platform supports runtime profile changes. |
| `US-QM-009` | Statistics Overlay can be toggled from Off to Basic/Advanced and back during a supported session without restarting the active application. |
| `US-QM-010` | A fresh/default user configuration starts applications with the Statistics Overlay disabled. |
| `US-QM-011` | Advanced mode displays only metrics backed by reliable platform/application data. Missing/unavailable metrics are omitted rather than fabricated. |
| `US-QM-012` | An application declaring streaming/media telemetry can register contextual metrics that appear only for that application/session. |
| `US-QM-013` | Brightness and volume changes can be performed controller-only from the Quick Menu and take effect immediately using device-global state. |
| `US-QM-014` | Actions marked `NOT APPLICABLE` for the active integration are not presented as normal usable controls. |
| `US-QM-015` | When a runtime option supports both temporary and persistent behavior, the UI explicitly identifies whether the change is `session only` or saved as a user/game default before persistence occurs. |
| `US-QM-016` | If contextual Advanced options are registered, they remain separated from the primary Quick Menu path and can be used without entering a separate desktop/raw application configuration UI. |

### Dependencies

| Dependency | Relationship |
|---|---|
| `EPIC-001` nuubUI | overlay shell, controller navigation and return semantics |
| `EPIC-003` Multi-user Profiles | ownership of persistent per-user/per-game runtime options |
| `EPIC-004` Settings | shared settings semantics and safe persistence |
| `EPIC-006` Notification System | success/error feedback for runtime operations |
| `EPIC-013` RetroArch Integration | save-state/runtime emulator integration |
| `EPIC-019` Saves & Save States | save-state behavior and safety |
| `EPIC-021` Screenshots & Capture | screenshot implementation and storage |
| `EPIC-022` Controller Management | mappings, order, player assignment and rumble |
| `EPIC-025` Moonlight / PC Game Streaming | streaming runtime controls/telemetry |
| `EPIC-026` Steam Remote Play | streaming runtime controls/telemetry |
| `EPIC-027` Network Media / Plex-style Client | media runtime controls/telemetry |
| `EPIC-028` Local Video & Music Player | media runtime controls/telemetry |
| `EPIC-043` Display / TV Mode | brightness/display runtime capabilities |
| `EPIC-044` Audio Management | volume/audio runtime capabilities |
| `EPIC-047` Performance Profiles | runtime performance-policy control |
| `EPIC-051` System Information | reusable runtime system metrics |
| `EPIC-052` Diagnostics & Support Bundle | advanced telemetry/diagnostic consistency |
| `ENABLER-004` Hardware Capability Baseline | CPU/GPU/thermal/input/display primitives |

### Platform / architecture requirements

EPIC-005 requires a stable userspace integration layer more than new kernel functionality.

| Area | Required capability |
|---|---|
| Overlay rendering | Ability to render nuubUI overlay content over or alongside the active application without corrupting its display lifecycle. |
| Input arbitration | A deterministic way to temporarily route controller input to the Quick Menu and return it to the application. |
| Application lifecycle | Stable pause/resume/restart/quit hooks or adapters per supported application class. |
| Emulator API/integration | Save-state, slot and contextual runtime controls exposed through a stable adapter rather than UI automation. |
| Metrics service | Access to reliable FPS/application metrics plus CPU frequency/load, temperature, memory and other available system metrics. |
| Performance control | Runtime access to the approved power/performance profiles. |
| Screenshot path | Stable capture mechanism for supported graphics/application paths. |
| Settings persistence | Explicit session-only versus persistent user/game overrides. |
| Controller service | Runtime mapping/player/rumble controls. |
| Notifications | Success/failure reporting for actions that are not instantaneous. |

### Current H700 support snapshot

This is a known-state snapshot, not the final capability audit.

| Capability | Current state | Notes |
|---|---|---|
| CPU frequency / DVFS data | `AVAILABLE` | Existing H700 platform exposes CPU DVFS. |
| GPU DVFS | `AVAILABLE` | Platform-final GPU power-management work exists. |
| Thermal sensors / thermal policy | `AVAILABLE` | Existing thermal coupling and power allocator can provide useful telemetry. |
| Performance profiles | `AVAILABLE AT PLATFORM LEVEL` | Auto / Performance / Battery Saver already exist; product API/UX still required. |
| Battery telemetry | `TO VALIDATE ON PHYSICAL BATTERY DEVICE` | Current RG35XX Pro dev unit has battery disconnected; CubeXX/40XX-V validation required. |
| Internal rumble | `AVAILABLE ON RG35XX PRO` | Product runtime integration not implemented. |
| External controller rumble | `PARTIAL / CONTROLLER DEPENDENT` | Xbox Model 1708 FF validated; other devices vary. |
| Screenshot product path | `NOT IMPLEMENTED` | Defined under EPIC-021. |
| Overlay compositor/rendering | `NOT IMPLEMENTED` | Depends on final nuubUI/application architecture. |
| FPS/frame-time telemetry | `NOT IMPLEMENTED AS COMMON SERVICE` | Must be supplied by application/render integration. |
| Streaming telemetry | `NOT IMPLEMENTED` | Depends on Moonlight/Steam integration. |
| Save-state bridge | `NOT IMPLEMENTED` | Depends on RetroArch/standalone emulator integration. |
| Quick Menu UI | `NOT IMPLEMENTED` | Product/userspace work. |

### Gaps identified

No new kernel patch is justified directly by this Epic.

Known architecture/userspace gaps:

1. define the Quick Menu invocation/input-arbitration mechanism;
2. define overlay rendering architecture compatible with selected nuubUI/application stack;
3. define a common application runtime adapter interface;
4. implement RetroArch save-state/slot runtime bridge;
5. define standalone-emulator adapters where required;
6. implement runtime performance-profile API;
7. implement a common metrics/telemetry service;
8. determine reliable FPS/frame-time sources per application/render stack;
9. integrate CPU/GPU/thermal/memory metrics;
10. integrate application-specific Moonlight/Steam/media telemetry;
11. implement screenshot integration;
12. implement session-only versus persistent override semantics;
13. implement runtime controller management integration;
14. validate overlay performance overhead on H700;
15. validate overlay operation on handheld and HDMI output.

### Decisions intentionally left open

The following remain open:

- exact Quick Menu invocation shortcut;
- whether opening the overlay pauses each application class;
- exact visual layout;
- exact overlay/compositor implementation;
- exact Basic statistics metric set after runtime validation;
- GPU utilization source if no reliable platform metric exists;
- exact performance-profile persistence UX wording;
- whether screenshots are stored per-user only or can optionally feed a shared gallery;
- exact application-specific runtime API/plugin mechanism.

### Refinement result

EPIC-005 is considered **REFINED**.

Approved product decisions include:

- a common controller-driven Quick Menu is part of nuubOS;
- context-sensitive actions only;
- Resume, Restart, Quit and Screenshot as core runtime actions;
- Save/Load State and slot control when supported;
- controller runtime controls;
- immediate brightness and volume;
- runtime Auto / Performance / Battery Saver selection;
- Statistics Overlay with `Off`, `Basic` and `Advanced`;
- Statistics Overlay is OFF by default;
- FPS/frame-time and platform performance telemetry are first-class diagnostics;
- streaming/media integrations may expose contextual latency/bitrate/decode metrics;
- unsupported/unreliable metrics are omitted rather than fabricated;
- session-only and persistent per-user/per-game overrides are explicitly distinguished.

---

## EPIC-006 — Notification System

**Status:** `REFINED`

### Product intent

nuubOS must provide a minimal, console-style notification system for transient events and long-running activities.

The notification system is **not** a persistent inbox and does not maintain a Notification Center.

It provides only two user-facing primitives:

```text
Notification System
├── Toast
└── Live Notification
```

Notifications communicate events and progress. They are **not** the authoritative storage of persistent system/application state.

If an important condition remains relevant after a notification disappears, that state must also remain visible in the owning feature.

Examples:

- update available → `Settings → Updates`;
- Syncthing error → Syncthing status/configuration;
- missing BIOS → BIOS Manager;
- Wi-Fi disconnected → Wi-Fi/status area;
- storage issue → Storage Manager.

### Scope

EPIC-006 owns:

- common visual/behavioral notification primitives;
- transient Toast notifications;
- mutable Live Notifications for long-running work;
- priority/severity semantics;
- optional contextual actions;
- progress/state update behavior;
- dismissal/lifetime rules;
- integration contract used by applications/services;
- controller-safe presentation;
- non-blocking display behavior.

EPIC-006 does **not** own:

- persistent notification history;
- Notification Center;
- background task scheduling/execution;
- authoritative application/system state;
- retry logic of the originating operation;
- logs/diagnostics;
- service-specific error handling.

Those responsibilities remain with the originating Epic/service.

### Notification types

## Toast

Toast notifications communicate short-lived events.

Typical examples:

```text
Controller connected
Wi-Fi connected
Screenshot saved
Save state created
Update available
Connection failed
Low battery
Bluetooth audio connected
```

A Toast:

- appears without replacing the current screen;
- remains visible for a short time;
- disappears automatically unless the event requires explicit acknowledgement;
- may expose at most a small number of contextual actions;
- must not interrupt controller focus unless the user explicitly enters the action.

Typical severity levels:

```text
INFO
SUCCESS
WARNING
ERROR
```

The exact iconography/colors are part of nuubUI/theme design rather than this Epic.

## Live Notification

A Live Notification represents an ongoing operation.

Typical examples:

```text
Scanning ROMs
234 / 1,492

ScreenScraper
Downloading artwork — 62%

Syncthing
Syncing saves — 8 files remaining

System Update
Downloading — 73%
```

A Live Notification is **updated in place**.

The system must not generate repeated Toast spam for every progress update.

A Live Notification may expose:

- operation name;
- current step/state;
- determinate progress;
- indeterminate progress;
- item counts;
- optional contextual action where appropriate.

When the operation completes, the Live Notification transitions into a short completion Toast/state, for example:

```text
✓ ROM scan completed — 1,492 games found
```

When it fails, it transitions into a concise failure notification, for example:

```text
⚠ ScreenScraper failed
Connection unavailable
```

Possible actions such as `Retry`, `Open` or `Details` are provided only when the originating feature supports them.

### No Notification Center

nuubOS intentionally does **not** implement a persistent Notification Center.

Consequences:

- notifications are transient UI;
- no unread count is required;
- no persistent notification list is required;
- no notification-history database is required;
- dismissing/expiring a notification does not erase the underlying system/application state;
- important persistent conditions must remain visible in their canonical owning screen.

This is a deliberate `NOT SUPPORTED BY DESIGN` product decision.

### Notification ownership

Each notification belongs to an originating feature/service.

Examples:

```text
ROM Scan       → Game Library / Background Jobs
ScreenScraper  → Metadata / ScreenScraper
Syncthing      → Syncthing
OTA            → Updates
Battery        → Battery & Charging
Wi-Fi          → Wi-Fi Manager
Controller     → Controller Management
Screenshot     → Screenshots & Capture
```

The notification layer only renders and routes the event.

### Severity and interruption

Notifications must not unnecessarily interrupt gameplay or media.

Default behavior:

- INFO/SUCCESS → passive;
- WARNING → visible but non-blocking;
- ERROR → visible and clear, but still non-blocking unless immediate user action is required;
- critical safety/power conditions may use stronger presentation through the owning Epic.

The normal Toast must not steal focus from an active game/application.

### Contextual actions

A notification may expose concise contextual actions when they materially help.

Examples:

```text
Update available     → Open
Screenshot saved     → Open
Sync failed          → Retry / Details
Wi-Fi disconnected   → Wi-Fi Settings
```

Actions must:

- be controller-accessible;
- invoke the canonical owning feature;
- never duplicate an entire settings flow inside the notification;
- never perform destructive actions without the confirmation semantics defined by the owning Epic.

### Deduplication and update behavior

Repeated identical state changes should not flood the user.

Examples:

- one controller reconnect should generate one event, not repeated duplicates;
- one ROM scan owns one Live Notification;
- progress updates mutate the same Live Notification;
- recurring transient network instability should be rate-limited/coalesced by the originating feature where appropriate.

The notification system must support stable notification/job identifiers so that a producer can update or replace its current notification.

### Foreground versus gameplay presentation

Notifications must work while:

- browsing nuubUI;
- running an emulator/game;
- running Moonlight/Steam streaming;
- playing media;
- using Web Mode where integration permits.

Presentation may adapt by context, but the semantic event remains the same.

During active gameplay, notifications should be compact and avoid covering critical screen areas as much as practical.

### User Stories

| ID | Priority | User Story |
|---|---|---|
| `US-NOT-001` | MUST | As a user, I want short events to appear as transient Toast notifications without interrupting what I am doing. |
| `US-NOT-002` | MUST | As a user, I want long-running operations to appear as Live Notifications that update their progress in place. |
| `US-NOT-003` | MUST | As a user, I want successful completion of a long-running activity to be communicated clearly without leaving a permanent notification behind. |
| `US-NOT-004` | MUST | As a user, I want failures to be clearly distinguishable from normal informational messages. |
| `US-NOT-005` | MUST | As a user, I want notifications to remain non-blocking during normal gameplay/media use. |
| `US-NOT-006` | MUST | As a user, I want repeated progress updates to update the same notification rather than flood the screen. |
| `US-NOT-007` | MUST | As a user, I want important persistent conditions to remain visible in the feature that owns them even after a notification disappears. |
| `US-NOT-008` | MUST | As a user, I want contextual notification actions such as Open/Retry/Details when the originating feature can support them safely. |
| `US-NOT-009` | MUST | As a user, I want notification actions to be fully usable with a controller. |
| `US-NOT-010` | MUST | As a user, I want notifications to work consistently both inside nuubUI and over supported games/applications. |
| `US-NOT-011` | MUST | As a user, I do not want a persistent Notification Center or unread-notification backlog added to the console experience. |

### Acceptance Criteria

| User Story | PASS condition |
|---|---|
| `US-NOT-001` | A producer can emit an INFO/SUCCESS/WARNING/ERROR Toast that appears without replacing the active screen and expires according to notification policy. |
| `US-NOT-002` | A producer can create a Live Notification with a stable identifier and subsequently update text/progress/state without creating a new notification for every update. |
| `US-NOT-003` | Completing a Live Notification transitions it to a short completion state/Toast and removes it automatically after the configured lifetime. |
| `US-NOT-004` | Failure notifications are visually/semantically distinguishable from success/info notifications and can include an actionable human-readable reason supplied by the originating feature. |
| `US-NOT-005` | Showing a normal notification does not steal controller focus from the current game/application or require acknowledgement before normal interaction continues. |
| `US-NOT-006` | Repeated updates for the same stable notification identifier replace/update the existing Live Notification rather than stacking duplicates. |
| `US-NOT-007` | At least one representative persistent condition (for example update available or sync failure) remains visible in its canonical feature after its Toast/Live Notification expires. |
| `US-NOT-008` | A producer may attach supported contextual actions; invoking an action opens/calls the owning feature and follows that feature's safety/confirmation rules. |
| `US-NOT-009` | Notification actions, when exposed, can be selected and invoked controller-only. Passive notifications require no interaction. |
| `US-NOT-010` | The notification surface can render during native nuubUI operation and during at least the officially supported foreground application classes selected by later Epics. |
| `US-NOT-011` | No persistent Notification Center, unread counter or notification-history screen is required or exposed as part of normal nuubOS behavior. |

### Dependencies

| Dependency | Relationship |
|---|---|
| `EPIC-001` nuubUI | common rendering, overlay behavior and controller navigation |
| `EPIC-005` Quick Menu / In-game Overlay | notification presentation while applications are active |
| `EPIC-007` Background Jobs & Downloads | primary producer of Live Notifications |
| `EPIC-015` Game Metadata & ScreenScraper | scrape progress/results |
| `EPIC-021` Screenshots & Capture | screenshot completion/failure |
| `EPIC-022` Controller Management | controller connection/disconnection notifications |
| `EPIC-034` Syncthing | synchronization progress/errors |
| `EPIC-040` Wi-Fi Manager | network status notifications |
| `EPIC-041` Bluetooth Manager | Bluetooth device events |
| `EPIC-046` Battery & Charging UX | low/critical battery and charging state events |
| `EPIC-048` OTA Updates | update availability/download/install progress |
| `EPIC-052` Diagnostics & Support Bundle | technical details remain outside ordinary notification text |
| `ENABLER-006` Logging & Persistent Diagnostics | logs retain diagnostic detail that notifications intentionally do not store |

### Platform / architecture requirements

EPIC-006 is a userspace/product capability.

| Area | Required capability |
|---|---|
| Notification service/API | Producers can create/update/complete/dismiss notifications through a stable interface. |
| Stable identifiers | Live Notifications can be updated by ID without duplication. |
| Overlay rendering | Notification surface can appear above supported foreground applications without breaking them. |
| Input separation | Passive notifications do not steal focus; contextual actions can deliberately acquire focus when invoked. |
| Application/session routing | Actions can open the canonical owning nuubUI feature. |
| Rate limiting/coalescing | Producer/service layer can prevent notification storms. |
| Localization | Notification text can use the active user's UI language. |
| Multi-user | User-specific notifications are routed only to the active user's session where applicable. |

### Current H700 support snapshot

This is a known-state snapshot, not the final capability audit.

| Capability | Current state | Notes |
|---|---|---|
| Kernel/userspace event sources | `AVAILABLE` | Wi-Fi, Bluetooth, power, input and other subsystems expose events usable by future services. |
| nuubUI notification renderer | `NOT IMPLEMENTED` | Depends on nuubUI stack. |
| Notification API/service | `NOT IMPLEMENTED` | Must be designed as userspace infrastructure. |
| Live progress model | `NOT IMPLEMENTED` | Closely tied to EPIC-007 Background Jobs. |
| Overlay notifications over applications | `NOT IMPLEMENTED` | Depends on final overlay/compositor architecture. |
| Persistent notification history | `NOT REQUIRED` | Explicitly excluded by design. |

### Gaps identified

No kernel change is justified directly by this Epic.

Known userspace/architecture gaps:

1. define common notification API/schema;
2. define stable notification identifiers;
3. implement Toast renderer/lifetime behavior;
4. implement Live Notification update/complete/failure lifecycle;
5. implement non-blocking overlay presentation;
6. implement context-aware placement during gameplay/media;
7. implement controller-safe contextual actions;
8. implement localization routing;
9. integrate active-user routing;
10. define rate-limit/coalescing rules;
11. integrate notification producers from later Epics;
12. validate notification overlay performance on H700.

### Decisions intentionally left open

The following remain open:

- exact Toast duration;
- exact on-screen placement;
- exact animations;
- exact iconography/colors;
- maximum number of simultaneously visible Toasts;
- exact number of contextual actions permitted by the UI layout;
- whether live progress is displayed as percentage, bar, count or adaptive form;
- how critical battery/system-safety alerts visually escalate beyond normal Toast behavior.

### Refinement result

EPIC-006 is considered **REFINED**.

Approved product decisions include:

- only two notification primitives: `Toast` and `Live Notification`;
- no Notification Center;
- no unread-notification model;
- no persistent notification history;
- notifications are transient and non-authoritative;
- persistent conditions remain visible in their owning feature;
- Live Notifications update in place;
- progress completion/failure transitions into short terminal notifications;
- notifications are non-blocking by default;
- contextual actions are allowed when useful and safe;
- notification rendering must work in nuubUI and supported foreground application contexts.

`Notification Center` is explicitly **NOT SUPPORTED BY DESIGN**.

---

## EPIC-007 — Background Jobs & Downloads

**Status:** `REFINED`

### Product intent

nuubOS must provide a common background-job framework for operations that take long enough that they should not block normal console use.

Typical examples:

```text
ROM scanning
ScreenScraper / metadata retrieval
artwork/media downloads
Syncthing-related user operations
OTA downloads
theme downloads
library maintenance
support-bundle generation
large copy/import operations where integrated
```

The framework is **not** a desktop-style task manager.

Jobs belong to the feature that created them. User-visible progress is surfaced through `EPIC-006 — Notification System`, primarily using Live Notifications.

Persistent state, error details and retry controls remain available in the canonical owning feature when relevant.

### Core principles

1. **Non-blocking by default**
   Long-running work should not freeze nuubUI when it can safely execute in the background.

2. **Feature-owned jobs**
   Every job has a clear owning Epic/service.

3. **Common lifecycle**
   Jobs expose a consistent state model regardless of producer.

4. **Live progress**
   User-visible progress is communicated through Live Notifications rather than a global task center.

5. **Explicit cancellation**
   A cancellable job must expose Cancel through its owning feature and/or Live Notification.

6. **Explicit retry**
   Failed work is retried only through defined product logic or a deliberate user action. The framework must not hide platform/software bugs behind infinite retry loops.

7. **Safe persistence**
   Only jobs that benefit from surviving process restart/reboot are persisted.

8. **Atomic outputs**
   Partial downloads or generated artifacts must not be mistaken for completed valid content.

9. **Bounded concurrency**
   Background work must not create uncontrolled CPU, memory, storage or network pressure.

10. **Suspend-aware behavior**
    Each job class declares what happens when the device enters suspend.

### Job lifecycle

Common job states:

```text
QUEUED
RUNNING
PAUSED          optional
CANCELLING      optional transitional state
COMPLETED
FAILED
CANCELLED
```

Optional persistent jobs may additionally support:

```text
INTERRUPTED
RESUMABLE
```

The exact internal representation is left open.

### Job identity

Every job must have:

- stable job identifier;
- owning feature/service;
- job type;
- active-user ownership when applicable;
- creation time;
- current state;
- human-readable title;
- optional progress;
- optional current step/status;
- cancellation capability flag;
- persistence/resume policy.

Stable job IDs allow a Live Notification to update in place.

### Job ownership

Examples:

| Job | Owning feature |
|---|---|
| ROM scan | Game Library / Automatic ROM Discovery |
| ScreenScraper metadata | Game Metadata & ScreenScraper |
| artwork/video download | Game Metadata & ScreenScraper |
| Syncthing user operation | Syncthing |
| OTA image download | OTA Updates |
| theme download/install | Themes |
| support bundle generation | Diagnostics & Support Bundle |
| BIOS import/validation | BIOS Manager |
| large content import | File/Storage/Library feature that initiated it |

The background framework never becomes the authoritative owner of feature-specific data.

### Queue and concurrency

Jobs may be queued.

The framework must provide a bounded concurrency model.

The exact limits may vary by job class, but nuubOS must prevent scenarios such as:

```text
50 parallel ScreenScraper downloads
+
OTA download
+
large file copy
+
multiple scans
```

from overwhelming H700 resources.

The scheduler may consider:

- CPU load;
- memory pressure;
- storage I/O;
- network usage;
- battery state;
- foreground activity;
- thermal state;
- job priority.

However, the initial implementation may use simpler fixed limits if they satisfy acceptance criteria.

### Priority classes

A small priority model is sufficient:

```text
USER_REQUESTED
NORMAL_BACKGROUND
MAINTENANCE
```

`USER_REQUESTED` means the user is actively waiting for or explicitly started the operation.

The framework must not introduce a complex desktop/job priority UI.

### Progress model

A job can expose:

#### Determinate progress

Examples:

```text
62%
234 / 1,492
8 / 23 files
```

#### Indeterminate progress

Used only where the total amount of work cannot be reliably known.

#### Step-based progress

Example:

```text
System Update
1. Checking
2. Downloading
3. Verifying
4. Preparing
```

The producer is responsible for accurate progress semantics.

The framework must not fabricate percentages.

### Live Notification integration

A running user-visible job should normally map to one Live Notification:

```text
job ID
  ↓
Live Notification ID
```

Updates mutate the same notification.

Terminal transition:

```text
COMPLETED
  → short success notification

FAILED
  → short error notification

CANCELLED
  → short cancellation notification only when useful
```

No persistent notification history is created.

### Cancellation

Cancellation is supported when technically safe.

A job explicitly declares:

```text
CANCELLABLE = yes/no
```

Examples likely cancellable:

- ROM scan;
- scraping;
- artwork downloads;
- some copy/import operations.

Examples that may become non-cancellable during critical phases:

- OTA installation;
- atomic metadata/database commit;
- destructive storage operation.

Cancellation must never leave completed-looking corrupt output.

Where required, cancellation performs cleanup before reaching `CANCELLED`.

### Retry

Failed jobs may offer:

```text
Retry
```

when retry is meaningful.

Retry behavior must be explicit.

Allowed examples:

- retry a failed HTTP download;
- retry scraping after network restoration;
- retry a user-requested import.

Not acceptable as production architecture:

- infinite retry loops;
- retry used to mask kernel/device initialization bugs;
- automatic process/service restart loops hiding deterministic failures.

Automatic retry may exist only where the owning protocol/application defines a bounded, legitimate transient-failure policy.

### Persistence and reboot behavior

Not every job must survive reboot.

Jobs are classified by persistence policy.

#### Ephemeral

The job does not need to survive reboot.

Examples may include:

- lightweight library scan;
- support-bundle generation;
- some local maintenance operations.

After interruption, the owning feature may safely restart the operation.

#### Restartable

The job state is known, but the operation restarts from the beginning after interruption.

#### Resumable

The job can persist enough validated state to continue safely.

Likely candidates:

- large OTA download;
- large content download;
- selected metadata/media download queues.

A job must be marked resumable only if continuation is verified to be safe.

### Partial downloads and atomic completion

Downloads must use a safe partial-content model.

Conceptually:

```text
target.partial
  ↓
download
  ↓
verify
  ↓
atomic publish / rename
  ↓
target
```

The exact filesystem mechanism depends on the owning feature.

A partial/incomplete file must never be presented as valid final content.

Where integrity metadata exists, verification occurs before publication.

This is mandatory for OTA update artifacts.

### Multi-user behavior

Jobs may be:

```text
DEVICE-SCOPED
USER-SCOPED
```

Examples:

#### Device-scoped

- OTA update;
- global ROM/library scan;
- shared metadata download;
- installed theme download.

#### User-scoped

- user-specific Syncthing operation;
- personal backup/export;
- user-scoped application data operation.

A user-scoped job must retain the identity of its owning user even if another user becomes active.

Switching user must not accidentally reassign the job.

Whether a user-scoped job continues after Switch User is defined by the owning Epic.

### Suspend behavior

Every job type must declare one of:

```text
PAUSE_ON_SUSPEND
CONTINUE_IF_PLATFORM_ALLOWS
BLOCK_SUSPEND_DURING_CRITICAL_PHASE
```

Default preference:

- ordinary background jobs pause or tolerate suspend;
- jobs must not keep the handheld awake indefinitely without an explicit reason;
- truly critical atomic phases may temporarily inhibit suspend.

The owning feature must communicate when suspend is temporarily unavailable.

### Network-aware behavior

Network jobs must react coherently to connectivity loss.

Expected behavior:

```text
network lost
  ↓
job enters waiting/failed state according to producer policy
  ↓
no notification spam
```

A job may resume automatically after connectivity returns only when the protocol and owning feature define that behavior safely.

Otherwise it fails clearly and exposes Retry.

### Storage-aware behavior

Before large downloads/copies, the owning feature should verify sufficient destination capacity when this can be determined reliably.

ENOSPC/storage removal must become a clear job failure rather than silent corruption.

Jobs writing removable storage must handle device removal safely.

### Resource-awareness

Background jobs must not make foreground gaming/media unusable.

The scheduler/framework must allow heavy background work to be constrained or paused when necessary.

Exact CPU/I/O scheduling techniques are implementation decisions.

The product requirement is observable:

> background maintenance must not cause unacceptable foreground responsiveness degradation under supported workloads.

### No global Task Center

nuubOS intentionally does **not** require a persistent global Background Task / Download Manager screen.

The primary user surfaces are:

```text
Live Notification
+
owning feature
```

A later Epic may expose a feature-specific queue where useful, for example ScreenScraper/library downloads.

A desktop-style universal Task Center is `NOT SUPPORTED BY DESIGN` unless future product requirements explicitly reopen the decision.

### User Stories

| ID | Priority | User Story |
|---|---|---|
| `US-JOB-001` | MUST | As a user, I want long-running operations to execute without blocking normal nuubUI use whenever technically safe. |
| `US-JOB-002` | MUST | As a user, I want running background work to expose clear status/progress through Live Notifications. |
| `US-JOB-003` | MUST | As a user, I want progress updates for one operation to update the same Live Notification rather than create duplicates. |
| `US-JOB-004` | MUST | As a user, I want to cancel operations that can be safely cancelled. |
| `US-JOB-005` | MUST | As a user, I want failed operations to explain failure and offer Retry when retry is meaningful. |
| `US-JOB-006` | MUST | As a user, I want interrupted downloads to never appear as valid completed content. |
| `US-JOB-007` | MUST | As a user, I want selected large operations such as OTA downloads to resume safely when their producer supports validated resume. |
| `US-JOB-008` | MUST | As a user, I want background activity to avoid making foreground gaming/media unusably slow. |
| `US-JOB-009` | MUST | As a user, I want network loss, storage-full and storage-removal failures to be handled cleanly rather than silently corrupting output. |
| `US-JOB-010` | MUST | As a multi-user device, we want user-scoped jobs to remain associated with the user that created them. |
| `US-JOB-011` | MUST | As a user, I want suspend behavior for an active job to be predictable and safe. |
| `US-JOB-012` | MUST | As a user, I do not want a desktop-style global Task Center added just to manage background jobs. |
| `US-JOB-013` | SHOULD | As a user, I want relevant owning features to expose their own queued/pending work when that adds real value. |

### Acceptance Criteria

| User Story | PASS condition |
|---|---|
| `US-JOB-001` | A representative long-running supported job can remain active while the user continues normal native nuubUI navigation without blocking the shell for the job's full duration. |
| `US-JOB-002` | A user-visible running job creates/updates one Live Notification containing at least current state and meaningful progress/status when available. |
| `US-JOB-003` | Multiple progress updates for one stable job ID mutate the existing Live Notification instead of stacking new notifications. |
| `US-JOB-004` | A job declared cancellable can be cancelled controller-only; cancellation reaches `CANCELLED` after required cleanup and does not publish incomplete output as final. |
| `US-JOB-005` | A failed job reaches `FAILED`, exposes a human-readable reason from the owning feature and offers Retry only when retry is declared safe/meaningful. |
| `US-JOB-006` | Interrupting a representative download before verification cannot leave the final destination artifact in a state considered valid/completed by the owning feature. |
| `US-JOB-007` | Every job type declared `RESUMABLE` is tested across process interruption/reboot and either resumes from validated state or safely falls back without accepting corrupt partial data. |
| `US-JOB-008` | Under defined representative foreground workloads, bounded background-job concurrency does not cause unacceptable UI/input responsiveness degradation or destabilize the foreground application. Exact performance thresholds are defined by the owning workload Epic/regression suite. |
| `US-JOB-009` | Representative network loss, ENOSPC and removable-storage interruption cases end in a defined recoverable state and never silently report successful completion. |
| `US-JOB-010` | A user-scoped job retains its original owner across user switching; another active user cannot accidentally inherit its credentials/output namespace. |
| `US-JOB-011` | Every registered job type declares a suspend policy. Suspend/resume testing confirms that the job either pauses/resumes safely, continues safely, or deliberately inhibits suspend during a documented critical phase. |
| `US-JOB-012` | Normal job operation is usable through Live Notifications and the owning feature without requiring a persistent global task-history/manager screen. |
| `US-JOB-013` | Where a feature-specific queue is implemented, it represents only that feature's work and uses the same common job lifecycle semantics. |

### Dependencies

| Dependency | Relationship |
|---|---|
| `EPIC-001` nuubUI | non-blocking foreground shell |
| `EPIC-003` Multi-user Profiles | job ownership and active-user routing |
| `EPIC-006` Notification System | Live Notification rendering/lifecycle |
| `EPIC-012` Automatic ROM Discovery | ROM scan jobs |
| `EPIC-015` Game Metadata & ScreenScraper | scraping/download queues |
| `EPIC-021` Screenshots & Capture | asynchronous post-processing if needed |
| `EPIC-031` Local File Manager | large file operations where background execution is supported |
| `EPIC-033` BIOS Manager | import/validation jobs |
| `EPIC-034` Syncthing | user-scoped synchronization tasks |
| `EPIC-035` Backup & Restore | backup/export/import jobs |
| `EPIC-048` OTA Updates | download/verification/install phases |
| `EPIC-052` Diagnostics & Support Bundle | support-bundle generation |
| `ENABLER-001` SYSTEM / STATE / USERDATA | persistent job/resume state |
| `ENABLER-003` Boot / Lifecycle Reliability | reboot/interruption behavior |
| `ENABLER-005` Security Baseline | credential isolation for network jobs |
| `ENABLER-007` Reliability / Watchdog / Failure Handling | failure semantics under interruption |

### Platform / architecture requirements

EPIC-007 is primarily common userspace infrastructure.

| Area | Required capability |
|---|---|
| Job service | Stable API to submit, observe, update, cancel and complete jobs. |
| Worker execution | Isolated/bounded worker execution that cannot block the nuubUI main loop. |
| Queue | Ordered/bounded pending-work model. |
| Identity | Stable job IDs and owner/user scope. |
| Persistence | Optional persistent job metadata/resume state outside immutable SYSTEM. |
| Atomic file operations | Safe partial/download output handling and atomic publication where filesystem semantics permit. |
| Resource control | Ability to bound worker/process concurrency and optionally adjust CPU/I/O priority. |
| Network state | Jobs can observe network availability/change events. |
| Storage state | Jobs can observe capacity/errors/removable-media disappearance. |
| Suspend integration | Job/service layer receives suspend/resume lifecycle and can request temporary inhibition for declared critical phases. |
| Notifications | Direct mapping from job lifecycle to Live Notifications. |
| Security | User-scoped network jobs execute with the correct user's credentials/namespaces. |

### Current H700 support snapshot

This is a known-state snapshot, not the final capability audit.

| Capability | Current state | Notes |
|---|---|---|
| Linux process/thread primitives | `AVAILABLE` | No platform blocker for background workers. |
| Persistent storage primitives | `AVAILABLE / FINAL LAYOUT TBD` | Final STATE/USERDATA schema still to be refined. |
| Network/Wi-Fi | `AVAILABLE` | Production Wi-Fi platform exists. |
| s2idle/resume | `AVAILABLE` | Job-level suspend behavior still needs userspace integration. |
| CPU/GPU thermal management | `AVAILABLE` | Useful for maintaining foreground stability under load. |
| Job service/framework | `NOT IMPLEMENTED` | Common userspace infrastructure required. |
| Persistent job queue | `NOT IMPLEMENTED` | Only needed for selected job types. |
| Live Notification bridge | `NOT IMPLEMENTED` | Depends on EPIC-006 implementation. |
| Resume-capable downloader | `NOT SELECTED/IMPLEMENTED` | Must be chosen per use case. |
| Resource-aware scheduling policy | `NOT IMPLEMENTED` | Initial bounded concurrency can be simpler. |

### Gaps identified

No kernel change is justified directly by this Epic.

Known architecture/userspace gaps:

1. define common job schema/API;
2. implement worker/process isolation from nuubUI;
3. implement bounded queue/concurrency control;
4. implement job ownership and per-user namespace routing;
5. integrate stable Live Notification IDs;
6. implement cancellation semantics;
7. implement explicit failure/retry semantics;
8. implement safe partial-download/atomic-publication helpers;
9. implement optional persistent/restartable/resumable job state;
10. integrate network and storage failure events;
11. integrate suspend/resume lifecycle;
12. define foreground-load protection/resource policy;
13. define representative H700 concurrency/performance validation;
14. integrate concrete producers as their Epics are refined.

### Decisions intentionally left open

The following remain open:

- exact job-service implementation technology;
- thread versus process worker architecture;
- exact queue scheduling algorithm;
- exact concurrency limits;
- exact persistence format;
- exact downloader/library used by OTA, themes and metadata;
- whether selected jobs continue after switching away from their owning user;
- exact resource thresholds for pausing/throttling background work;
- exact suspend-inhibition API;
- exact feature-specific queue UIs.

### Refinement result

EPIC-007 is considered **REFINED**.

Approved product decisions include:

- long-running work uses a common background-job framework;
- normal nuubUI remains usable during background work where technically safe;
- one stable job maps to one updating Live Notification;
- jobs belong to their originating feature;
- safe cancellation where supported;
- explicit/bounded retry semantics;
- no retry loops used to hide platform bugs;
- bounded concurrency;
- partial files never appear as completed valid content;
- persistence/resume is opt-in by job type, not universal;
- OTA/large-download resume is supported when safely implementable and validated;
- job ownership can be device-wide or per-user;
- every job type has a declared suspend policy;
- no desktop-style global Task Center is required;
- feature-specific queues are allowed where genuinely useful.

`Global Task Center` is explicitly **NOT SUPPORTED BY DESIGN** for the current product contract.

---

## EPIC-008 — Themes

**Status:** `REFINED`

### Product intent

nuubOS must support a simple, documented and safe theming system for nuubUI.

A theme must be installable and selectable **without recompiling nuubUI**.

The theming model should make it practical for users and community creators to build themes using a clear package structure and documented assets/configuration rather than modifying product source code.

Theme selection is **per-user** as defined by EPIC-003.

Installed theme packages are **device-global/shared** so the same theme package is not duplicated for every user.

### Core principles

1. **No nuubUI rebuild required**
   Installing or changing a theme must not require recompiling the frontend.

2. **Documented format**
   Theme structure, supported assets, variables and compatibility rules must be documented.

3. **Simple authoring model**
   A basic theme should be creatable by replacing/configuring documented assets and style properties without writing application code.

4. **Per-user selection**
   Each persistent user may choose a different installed theme.

5. **Shared installation**
   Theme packages are installed once device-wide and referenced by user profiles.

6. **Safe fallback**
   An invalid, incompatible or broken theme must never prevent nuubUI from starting.

7. **Preview before activation**
   Users should be able to preview a theme before committing it as their active theme.

8. **Versioned compatibility**
   Theme packages must declare enough metadata for nuubUI to determine compatibility.

9. **No executable theme code by default**
   Themes are presentation packages, not arbitrary executable plugins.

10. **Licensing/provenance awareness**
    Theme assets, fonts and other redistributed resources must have known redistribution rights before inclusion in official nuubOS distributions.

### Theme package concept

The exact on-disk format remains open, but conceptually a theme package contains:

```text
theme-name/
├── manifest
├── styles / variables
├── fonts
├── icons
├── images
├── backgrounds
├── sounds              optional
├── previews
└── other declared presentation assets
```

A theme package must not require direct modification of nuubUI source files.

### Theme manifest

Every installable theme must expose metadata equivalent to:

```text
Theme ID
Display name
Author
Version
Theme format version
Minimum/compatible nuubUI version
Description
License / attribution metadata
Preview asset
Optional feature flags
```

The exact serialization format is intentionally left open.

`Theme ID` must be stable and distinct from the human-readable name.

### Supported customization areas

The final theme specification is deferred until nuubUI technology is chosen, but the theme system is expected to cover presentation areas such as:

- colors;
- typography;
- fonts;
- backgrounds;
- cards/tiles;
- focus/selection presentation;
- icons;
- spacing/density within safe limits;
- system/library artwork framing;
- notification appearance;
- Quick Menu appearance;
- transitions/animation parameters where explicitly supported;
- optional UI sounds where the audio/product design allows them.

Themes must not be allowed to change product semantics.

Examples:

- a theme may change how the `Power` action looks;
- it may not remove the mandatory `Power` functionality;
- a theme may style focus;
- it may not make focus semantically absent;
- a theme may alter typography;
- it may not make mandatory text unreadable.

### Layout customization

Theme layout customization should be powerful enough to support distinct visual identities, but product usability constraints remain authoritative.

A theme must not:

- remove mandatory navigation paths;
- hide mandatory status/error information;
- make controller focus impossible to determine;
- require touch/mouse for ordinary operation;
- bypass accessibility/safe-layout constraints defined by nuubUI.

The exact balance between style variables and layout templates will be decided after the nuubUI framework is selected.

### Installation

Themes may be installed through supported product flows such as:

- local file import;
- removable storage;
- Web administration;
- future theme repository/download mechanism.

At minimum, nuubOS must validate:

- package structure;
- manifest presence;
- supported theme-format version;
- required assets/configuration;
- compatibility metadata.

An invalid package must not replace the currently working theme.

### Import / Export

Theme packages should be portable.

Users/creators should be able to:

- import a valid theme package;
- export/share a locally available theme package where redistribution rights permit;
- reinstall the same package on another compatible nuubOS device.

Export must not silently include proprietary/user-private data not part of the original theme.

### Preview

Before activation, nuubUI should provide a preview using representative UI elements.

The preview should demonstrate enough of the theme to evaluate:

- typography;
- colors;
- selection/focus state;
- major component styling;
- Home/library presentation.

A preview must not permanently switch the active theme until the user confirms.

### Activation

Theme selection belongs to the active user profile.

Conceptually:

```text
Installed Themes                  DEVICE-GLOBAL

User Fabio
└── Active theme: Theme A

User Elisa
└── Active theme: Theme B
```

Switching user loads that user's selected theme.

If the selected theme is no longer available or becomes incompatible, nuubUI falls back safely.

### Default theme

nuubOS must always ship with at least one built-in/default theme that:

- is known-good;
- is compatible with the current nuubUI version;
- cannot be accidentally removed through normal theme management;
- provides the safe fallback path.

The default theme is part of the product baseline.

### Safe fallback / broken-theme recovery

Theme loading must be fail-safe.

If any of the following occurs:

- invalid manifest;
- incompatible format version;
- missing critical asset;
- corrupted theme package;
- theme load/render initialization failure;
- selected theme removed;
- update makes an old theme incompatible;

then nuubUI must:

```text
detect failure
  ↓
fall back to built-in safe/default theme
  ↓
remain navigable
  ↓
inform the user non-blockingly
```

A broken theme must never create a boot loop.

The recovery path integrates with EPIC-050.

### Theme updates

Themes may support updates.

A theme update must:

- preserve the previous working version until the new package validates;
- not switch to a broken package atomically;
- respect compatibility requirements;
- provide failure feedback.

Automatic theme updates are not required.

### Theme removal

Users may remove installed non-default themes.

Removal must:

- never remove the built-in safe theme;
- detect users currently referencing the theme;
- reassign affected users to the default theme or require an explicit replacement choice;
- never leave a profile with an invalid active-theme reference.

### Fonts

Themes may include fonts if the final theme specification supports it.

Bundled fonts must satisfy:

- redistribution/license requirements;
- platform-supported font format;
- readability constraints;
- reasonable memory/storage constraints.

nuubOS must retain a known-good fallback font independent of third-party themes.

### Theme sounds

Optional theme sounds may be supported if the future nuubUI/audio design includes them.

They are not required for the baseline theme format.

If supported:

- they must respect global/user audio policy;
- they must not interfere with game/media audio;
- absence of sound assets must never invalidate an otherwise valid theme.

### Theme repository / discovery

An online community theme repository is **OPTIONAL / future-capable**, not a baseline requirement.

The baseline requirement is a portable, installable theme package.

A future repository may add:

- browse;
- download;
- updates;
- ratings/previews;
- compatibility filtering.

This must not be required to use local/custom themes.

### User Stories

| ID | Priority | User Story |
|---|---|---|
| `US-THM-001` | MUST | As a user, I want to change nuubUI's appearance without recompiling or modifying nuubUI source code. |
| `US-THM-002` | MUST | As a theme creator, I want a documented theme format so that I can create compatible themes without reverse engineering the frontend. |
| `US-THM-003` | MUST | As a user, I want to install/import theme packages through a supported product flow. |
| `US-THM-004` | MUST | As a user, I want to preview a theme before making it active. |
| `US-THM-005` | MUST | As a multi-user household, we want each user to select a different active theme while sharing the installed theme packages. |
| `US-THM-006` | MUST | As a user, I want nuubOS to fall back automatically to a safe default theme if my selected theme is broken or incompatible. |
| `US-THM-007` | MUST | As a user, I want an invalid theme installation to leave my current working theme untouched. |
| `US-THM-008` | MUST | As a user, I want to remove non-default themes without making any profile unusable. |
| `US-THM-009` | MUST | As a user, I want themes to preserve mandatory controller navigation, focus visibility and core product functions. |
| `US-THM-010` | MUST | As a project maintainer, I want theme packages to declare identity/version/compatibility and licensing metadata. |
| `US-THM-011` | SHOULD | As a theme creator, I want themes to be portable/importable/exportable between compatible nuubOS devices. |
| `US-THM-012` | SHOULD | As a user, I want installed themes to be updatable safely. |
| `US-THM-013` | OPTIONAL | As a user, I want a future online theme repository for browsing/downloading compatible community themes. |

### Acceptance Criteria

| User Story | PASS condition |
|---|---|
| `US-THM-001` | A valid theme package can be installed and activated on a normal nuubOS image without recompiling nuubUI or modifying SYSTEM source files. |
| `US-THM-002` | The repository contains a documented theme specification sufficient to build a basic theme package and validate its manifest/assets. |
| `US-THM-003` | A controller-driven supported import/install path validates a theme before publication and reports success/failure clearly. |
| `US-THM-004` | A user can preview a valid installed/imported theme and cancel without altering the persistent active-theme selection. |
| `US-THM-005` | Two users can select different themes from the same device-global installed theme set; switching users applies the target user's theme. |
| `US-THM-006` | Corrupting/removing/incompatibilizing the active third-party theme results in successful nuubUI startup using the built-in safe theme rather than a crash/boot loop. |
| `US-THM-007` | Installing an invalid/incompatible theme cannot overwrite/delete the active working theme or change the user's active-theme reference. |
| `US-THM-008` | The built-in default theme cannot be removed through normal UI; removing another theme leaves all affected users referencing a valid theme. |
| `US-THM-009` | A validated theme cannot remove mandatory Home/Back/Power/navigation semantics or make controller focus semantically unavailable. |
| `US-THM-010` | Every accepted theme exposes stable ID, version, theme-format compatibility and required attribution/license metadata fields. |
| `US-THM-011` | If export is implemented, exporting/importing the same valid package on a compatible device reproduces the theme without requiring source changes. |
| `US-THM-012` | If theme update is implemented, the new package is validated before replacing the previous working package and failed update leaves the old version usable. |
| `US-THM-013` | If an online repository is later implemented, local/offline theme import remains independently usable. |

### Dependencies

| Dependency | Relationship |
|---|---|
| `EPIC-001` nuubUI | renderer/component/layout contract consumed by themes |
| `EPIC-003` Multi-user Profiles | per-user active-theme selection |
| `EPIC-004` Settings | theme selection/management UI |
| `EPIC-006` Notification System | install/update/fallback feedback |
| `EPIC-007` Background Jobs & Downloads | optional theme download/update jobs |
| `EPIC-009` Localization | themed typography must support localized UI |
| `EPIC-010` Accessibility & Usability | theme usability/readability constraints |
| `EPIC-048` OTA Updates | theme-format compatibility may change across product updates |
| `EPIC-050` Crash Recovery / Safe Mode | recovery from broken theme state |
| `ENABLER-001` SYSTEM / STATE / USERDATA | installed theme packages and per-user selection storage |
| `ENABLER-005` Security Baseline | safe package parsing/no arbitrary executable theme code |
| `ENABLER-008` Licensing / Provenance / Redistribution | official theme/font/asset redistribution compliance |

### Platform / architecture requirements

EPIC-008 is a userspace/UI capability.

| Area | Required capability |
|---|---|
| Theme loader | Versioned theme package parser with validation and fallback behavior. |
| Resource resolution | Deterministic loading of theme assets without overwriting immutable product resources. |
| Per-user settings | Active-theme ID stored in the user's persistent profile. |
| Shared storage | Installed theme packages stored once device-wide. |
| Safe default | Built-in immutable/default theme always available. |
| Compatibility | nuubUI/theme-format version checks before activation. |
| Atomic installation | Validated package published atomically; failed install/update preserves working version. |
| Package security | Theme packages cannot execute arbitrary code by default and cannot escape permitted asset/config paths. |
| Font/rendering | Selected nuubUI stack must support required bundled/default/theme fonts within platform constraints. |
| Recovery | Theme-load failure can force safe/default theme before normal shell startup completes. |

### Current H700 support snapshot

This is a known-state snapshot, not the final capability audit.

| Capability | Current state | Notes |
|---|---|---|
| Filesystem/storage primitives | `AVAILABLE` | No platform blocker to storing shared theme packages. |
| Per-user persistence concept | `DEFINED / NOT IMPLEMENTED` | EPIC-003 defines theme selection as per-user. |
| nuubUI rendering/theme API | `NOT IMPLEMENTED` | Depends on final nuubUI technology. |
| Theme package format | `NOT DEFINED` | Must be designed after UI stack selection. |
| Theme validator | `NOT IMPLEMENTED` | Userspace work. |
| Built-in safe theme | `NOT IMPLEMENTED` | Must ship with nuubUI. |
| Preview renderer | `NOT IMPLEMENTED` | Product/userspace work. |
| Theme import/export | `NOT IMPLEMENTED` | Product/userspace work. |
| Licensing pipeline | `PARTIALLY DEFINED AS PROJECT RULE` | Final audit tracked under ENABLER-008. |

### Gaps identified

No kernel change is justified by this Epic.

Known userspace/product gaps:

1. select final nuubUI rendering/component architecture;
2. define the theme package/manifest schema;
3. define style variables and safe layout customization surface;
4. define theme format versioning/compatibility rules;
5. implement package validation;
6. implement built-in immutable safe theme;
7. implement per-user active-theme selection;
8. implement preview;
9. implement atomic install/remove/update behavior;
10. implement fallback on load/render failure;
11. define font support and fallback behavior;
12. define license/attribution metadata requirements for themes;
13. document theme authoring with a minimal example/template;
14. optionally implement import/export and future online repository.

### Decisions intentionally left open

The following remain open:

- exact theme manifest serialization;
- exact package/archive format;
- exact theme storage path;
- exact set of customizable variables/components;
- degree of layout customization;
- exact font formats;
- support for optional theme sounds;
- online theme repository/provider;
- whether theme packages are cryptographically signed for official distribution;
- exact preview scene/content;
- exact compatibility policy across major/minor nuubUI versions.

### Refinement result

EPIC-008 is considered **REFINED**.

Approved product decisions include:

- themes can be installed/changed without rebuilding nuubUI;
- theme format is documented and versioned;
- installed themes are device-global;
- active theme selection is per-user;
- preview before persistent activation;
- built-in known-good theme is always available;
- invalid/broken/incompatible themes fall back safely and cannot boot-loop nuubUI;
- theme packages do not contain arbitrary executable code by default;
- themes cannot remove mandatory product/navigation semantics;
- package install/update is validated and atomic;
- fonts/assets used by official themes require license/provenance tracking;
- local theme packages are baseline functionality;
- an online theme repository remains optional/future.

---

## EPIC-009 — Localization & Regional Settings

**Status:** `REFINED`

### Product intent

nuubOS must provide a localized console experience while keeping regional/device settings simple and predictable.

UI language is a **per-user** preference.

Timezone and system clock are **device-global**.

The product must remain usable even when a translation is incomplete.

### Approved ownership model

#### Per-user

- UI language;
- locale-aware presentation preferences where derived from language/locale;
- accessibility-related language presentation preferences where applicable.

#### Device-global

- timezone;
- RTC/system time;
- NTP/automatic time;
- physical keyboard layout where configured as a device/input setting.

### Language behavior

Each user may select a different nuubUI language.

Switching user applies the target user's configured UI language without changing the language selected by other profiles.

At minimum, nuubOS must always provide a complete built-in English fallback.

Missing translations must never produce blank labels, broken navigation or unusable screens.

Fallback rule:

```text
selected language translation
  ↓ if missing
English source/fallback string
```

### Regional formatting

Date/time and other locale-sensitive presentation should follow the active user's selected language/locale where appropriate, while the underlying system clock remains device-global.

Examples:

- date presentation;
- 12/24-hour formatting;
- decimal/grouping conventions if later needed;
- localized names for months/days;
- localized UI labels.

The exact locale model remains open until the UI/localization framework is selected.

### Timezone / Date & Time

Timezone and system time remain device-global and are managed by EPIC-053.

EPIC-009 only defines their localized presentation.

Automatic NTP behavior approved in EPIC-002 remains unchanged.

### Keyboard layout

External keyboard layouts must be selectable through Settings.

The existing platform facility already includes multiple `loadkeys` layouts.

Keyboard layout is treated primarily as a **device/input setting**, not a personal translation setting, unless later product requirements justify per-user keyboard layouts.

Normal nuubUI operation remains controller-first and never requires a keyboard.

### Translation architecture

User-facing product strings must not be hard-coded throughout application logic in a way that prevents localization.

The selected framework must support:

- stable localization keys;
- parameterized strings;
- plurals where required;
- safe fallback;
- runtime user-language switching where technically practical;
- translation validation.

Raw technical/kernel errors shown only in Advanced/Diagnostics are not required to be fully product-localized in the same way as ordinary UI, though their surrounding explanations should be.

### Content versus UI localization

EPIC-009 owns nuubOS product-interface localization.

It does not require translation of third-party game metadata, web content, emulator UIs or externally supplied media metadata.

Where third-party content already provides localized fields, later Epics may choose the best available language.

### User Stories

| ID | Priority | User Story |
|---|---|---|
| `US-L10N-001` | MUST | As a user, I want to choose my nuubOS UI language independently from other users. |
| `US-L10N-002` | MUST | As a user, I want missing translations to fall back safely to English instead of breaking the interface. |
| `US-L10N-003` | MUST | As a user, I want dates/times and other regional presentation to be understandable in my selected locale. |
| `US-L10N-004` | MUST | As a device owner, I want timezone/system time to remain device-global even when different users use different UI languages. |
| `US-L10N-005` | MUST | As a user, I want to select a supported external keyboard layout when using a physical keyboard. |
| `US-L10N-006` | MUST | As a maintainer, I want UI strings to use a localization framework with stable keys and validation rather than scattered hard-coded text. |
| `US-L10N-007` | SHOULD | As a user, I want language changes to apply without a full device reboot where the selected UI framework supports it safely. |

### Acceptance Criteria

| User Story | PASS condition |
|---|---|
| `US-L10N-001` | Two users can select different supported UI languages; switching profiles loads each user's own language without modifying the other profile. |
| `US-L10N-002` | Removing/omitting a non-English translation for a representative UI string results in readable English fallback text and preserves navigation/functionality. |
| `US-L10N-003` | Representative date/time values are rendered according to the active locale/presentation rules without altering the underlying system time. |
| `US-L10N-004` | Changing the active user/language does not change the configured device timezone or RTC/system clock. |
| `US-L10N-005` | A supported keyboard layout can be selected and applied through controller-driven Settings, and an external keyboard produces the expected layout afterward. |
| `US-L10N-006` | Ordinary nuubUI strings are resolved through the chosen localization mechanism and the build/test process can detect missing/invalid localization entries. |
| `US-L10N-007` | If runtime switching is supported, changing language updates the active session without reboot and without leaving mixed/broken UI state; otherwise a clearly defined shell restart/reload is sufficient. |

### Dependencies

| Dependency | Relationship |
|---|---|
| `EPIC-001` nuubUI | localized shell/rendering |
| `EPIC-002` First Boot / Onboarding | localized first-boot presentation where available |
| `EPIC-003` Multi-user Profiles | per-user UI language ownership |
| `EPIC-004` Settings | language/keyboard configuration |
| `EPIC-008` Themes | fonts/layouts must support localized text |
| `EPIC-010` Accessibility & Usability | readability and text-scaling constraints |
| `EPIC-023` Keyboard & Mouse | external keyboard support |
| `EPIC-053` Date / Time / RTC | timezone/system clock |
| `ENABLER-001` SYSTEM / STATE / USERDATA | per-user language persistence |
| `ENABLER-008` Licensing / Provenance / Redistribution | bundled fonts/translation assets where applicable |

### Platform / architecture requirements

EPIC-009 is primarily userspace/product infrastructure.

| Area | Required capability |
|---|---|
| Localization framework | Stable keys, fallback, parameterization and plural support where needed. |
| Font fallback | Built-in font stack must render all officially supported UI languages. |
| Per-user persistence | Selected language stored in user profile. |
| Locale data | Required locale/date/time formatting data available in the image/userspace. |
| Timezone data | Timezone database/support available through EPIC-053. |
| Keyboard layouts | Supported keymaps available and switchable through the input/system layer. |
| Validation | Build/test tooling detects malformed/missing translation resources. |

### Current H700 support snapshot

| Capability | Current state | Notes |
|---|---|---|
| Multiple local keyboard layouts | `AVAILABLE` | `kbd`, `loadkeys` and multiple keymaps already validated. |
| User-language model | `DEFINED / NOT IMPLEMENTED` | Per-user language approved by EPIC-003. |
| Localization framework | `NOT SELECTED` | Depends on nuubUI technology. |
| English fallback content | `NOT IMPLEMENTED` | Must be baseline of final UI. |
| Locale/timezone product integration | `TO IMPLEMENT / AUDIT` | EPIC-053 will define system time details. |
| Font coverage | `TO DEFINE` | Depends on default/theme font architecture. |

### Gaps identified

No kernel change is justified directly by this Epic.

Known userspace/product gaps:

1. select localization framework with nuubUI technology;
2. define supported launch languages;
3. define stable translation-key conventions;
4. implement English fallback;
5. define locale/date/time formatting policy;
6. integrate per-user language switching;
7. ensure default font/fallback fonts cover supported languages;
8. integrate external keyboard layout selection;
9. add translation validation/build tooling;
10. define translation contribution workflow/documentation.

### Decisions intentionally left open

The following remain open:

- exact initial language list;
- exact locale identifier model;
- whether language and locale are one selector or separate advanced settings;
- exact runtime language reload mechanism;
- exact translation file format;
- community translation workflow;
- per-user keyboard layout support if ever needed;
- localized third-party metadata selection policy.

### Refinement result

EPIC-009 is considered **REFINED**.

Approved product decisions include:

- UI language is per-user;
- timezone/system time are device-global;
- external keyboard layout is device/input configuration;
- English is the mandatory fallback language;
- missing translations never break normal UI;
- locale-aware presentation follows the active user's language/locale where appropriate;
- UI strings use a proper localization architecture;
- normal console operation remains controller-first regardless of language/input configuration.

---

## EPIC-010 — Accessibility & Usability

**Status:** `REFINED`

### Product intent

For the current CFW scope, nuubOS does not target a broad desktop-style accessibility feature set.

The baseline requirement is intentionally small:

> nuubUI must scale correctly and remain usable on every officially supported display mode.

### Scope

nuubUI must provide:

- correct UI scaling;
- correct text scaling;
- no essential element rendered outside the usable screen;
- no overlap or clipping that makes normal UI unusable;
- correct behavior on the integrated handheld display;
- correct behavior on officially supported external display / TV modes;
- unchanged controller navigation semantics across supported resolutions.

Focus visibility and general controller usability remain requirements of `EPIC-001 — nuubUI` and are not duplicated here.

### Out of current product scope

The current CFW baseline does not require:

- screen reader;
- text-to-speech;
- voice control;
- magnifier;
- color-vision filters;
- high-contrast mode;
- reduce-motion mode;
- accessibility-specific haptics/audio;
- complex desktop accessibility frameworks.

These capabilities may be introduced later through new/refined requirements if product scope changes.

### User Stories

| ID | Priority | User Story |
|---|---|---|
| `US-ACC-001` | MUST | As a user, I want nuubUI to scale correctly on every officially supported display mode so that the interface remains readable and usable. |

### Acceptance Criteria

| User Story | PASS condition |
|---|---|
| `US-ACC-001` | On every officially supported integrated/external display mode, normal nuubUI screens remain usable with correct UI/text scaling, no essential controls outside the usable area, and no clipping/overlap that prevents controller-driven operation. |

### Dependencies

| Dependency | Relationship |
|---|---|
| `EPIC-001` nuubUI | responsive UI/layout implementation |
| `EPIC-008` Themes | themes must remain inside supported scaling/layout constraints |
| `EPIC-009` Localization & Regional Settings | localized strings must remain usable under supported scaling |
| display-related Epics/Enablers | define officially supported integrated/external display modes |

### Platform / architecture requirements

EPIC-010 is a userspace/UI requirement.

The selected nuubUI stack must provide a deterministic responsive/scaling strategy for supported handheld and external-display resolutions.

### Current H700 support snapshot

| Capability | Current state | Notes |
|---|---|---|
| nuubUI responsive layout | `NOT IMPLEMENTED` | Depends on final UI stack. |
| Integrated display validation | `BLOCKED ON CURRENT RG35XX PRO DEV UNIT` | LCD is physically disconnected on the current development unit. |
| External display validation | `TO AUDIT` | Final supported modes still need qualification. |

### Gaps identified

No kernel change is justified directly by this Epic.

Required work:

1. define officially supported display modes;
2. implement responsive/scaled nuubUI layout;
3. validate representative screens on each supported mode;
4. include scaling/layout checks in UI regression testing.

### Refinement result

EPIC-010 is considered **REFINED** with intentionally minimal CFW scope.

---

## EPIC-011 — Game Library

**Status:** `REFINED`

### Product intent

The Game Library is the primary catalog of games known to nuubOS.

ROM/content identity and base metadata are shared device-wide. Personal organization and activity are layered per-user.

The experience must remain console-first:

> selecting a game and pressing `A` launches it immediately.

Game details and management actions are available through a separate controller action/context.

### Primary structure

```text
Game Library
├── All Games
├── Systems
├── Favorites
├── Recently Played
├── Custom Collections
└── Search
```

### Immediate launch

On a normal game entry:

```text
A
  ↓
Launch game
```

Opening a separate details page is **not** required before launch.

A different documented controller action opens Game Details.

### Game list/card information

The normal library presentation must expose enough information to make browsing useful without opening Details.

At minimum, where applicable, the entry exposes:

- game title;
- system/platform context;
- availability state;
- `Last Played`;
- `Time Played`.

`Last Played` and `Time Played` are therefore baseline Library presentation data, even though their authoritative tracking/statistics service is owned by the relevant play-history/statistics Epic.

Never-played games use a clear neutral representation rather than fabricated values.

### Systems

Games discovered by EPIC-012 are automatically associated with supported systems.

The user does not manually create platform categories for ordinary ROM discovery.

### All Games

`All Games` provides an aggregated view across supported systems.

Baseline sorting includes:

- A → Z;
- Z → A;
- Recently Played;
- Most Played;
- Recently Added where discovery timestamps are available.

### Favorites

Favorites are **per-user**.

The same shared game may be favorited by one user and not another.

Favorite toggle must be fast and controller-driven.

### Recently Played

Recently Played is generated automatically and is **per-user**.

At minimum, the Library consumes:

- last-played timestamp;
- cumulative time played.

### Custom Collections

Custom collections are **per-user** and may contain games from different systems.

Collections store references to games, not duplicate ROM files.

### Search

Search is cross-library and must support at least title matching.

It is controller-usable through the nuubOS virtual keyboard and may additionally accept a physical keyboard.

### Random Game / Surprise Me

Random selection is a baseline `MUST`.

The command selects one launchable game from the current logical browsing context.

Examples:

```text
All Games      → random from all eligible games
System         → random from that system
Favorites      → random favorite
Collection     → random from that collection
Search results → random from the current result set
```

The random candidate set must exclude:

- `UNAVAILABLE` games;
- games hidden for the active user;
- entries that cannot currently be launched.

Random selection chooses a game; the exact final UX may either highlight/reveal the selected game or launch it according to the final interaction design. It must not silently select an invalid entry.

### Game Details

A dedicated controller action opens the complete Game Details view.

Where metadata/capabilities exist, it may show:

- title;
- system;
- cover/artwork;
- description;
- release information;
- developer;
- publisher;
- genre;
- player count;
- screenshots/video;
- Last Played;
- Time Played;
- achievements information;
- availability/path/storage information where appropriate.

Contextual actions may include:

- Play;
- Favorite;
- Add/Remove from Collection;
- Hide Game;
- Delete Game;
- Game Settings;
- Save States;
- Achievements;
- other capability-specific actions.

Actions are shown only when applicable.

### Metadata independence

The Library must work offline and without ScreenScraper/online metadata.

A discovered ROM must remain browseable and launchable using locally derivable information such as its filename/title and system.

Online metadata enriches the experience but is never required for a functional Library.

### Availability model

A known game has an explicit availability state.

Baseline:

```text
AVAILABLE
UNAVAILABLE
```

When backing ROM/storage disappears:

```text
AVAILABLE
  ↓
ROM/storage disappears
  ↓
UNAVAILABLE
```

The Library retains the game entry and associated information.

It must retain, where applicable:

- base metadata;
- artwork references/cache;
- favorites;
- custom collection membership;
- Last Played;
- Time Played;
- saves/states;
- per-game configuration.

An unavailable game is **not silently deleted** from the catalog.

The UI must clearly communicate `UNAVAILABLE`.

If the same known content becomes available again through the discovery/storage layer, the entry returns to `AVAILABLE` without losing its retained state.

### Hidden games

`Hide Game` is **per-user**.

Hiding a game:

- does not delete the ROM;
- does not affect other users;
- removes it from normal browsing for that user;
- preserves all game/user data;
- can be reversed through Library settings/hidden-game management.

### Delete Game / ROM

The Game Library must allow a user to permanently delete a game and its backing ROM/content directly from the game-management UI.

This is a **device-global destructive operation**.

The UI must explicitly state that the backing content will be permanently deleted and that the game will cease to be available to all users.

Deletion requires explicit confirmation.

Conceptually:

```text
Delete "Crash Bandicoot"?

This permanently deletes the game ROM
from storage for all users.

[Cancel] [Delete]
```

Successful deletion removes the backing ROM/content according to EPIC-012 ownership rules.

The Library must never claim successful deletion if filesystem deletion failed.

Per-user data retention/cleanup after deliberate permanent ROM deletion must follow an explicit policy; it must not accidentally delete unrelated saves or shared data.

### Multi-file / multi-disc deletion safety

Some logical games reference multiple files.

Examples include:

- `.m3u` multi-disc sets;
- cue/bin sets;
- other descriptor + payload arrangements.

The Library must not guess destructively.

EPIC-012 must provide a validated content ownership/reference model identifying which files belong to the logical game and whether any are shared/referenced elsewhere.

Only files proven safe to delete may be included in a destructive game deletion.

Ambiguous/shared files must not be deleted silently.

### Multi-disc representation

A supported multi-disc title should appear as one logical Library game when EPIC-012 identifies it as such.

Example:

```text
Final Fantasy VII.m3u
├── Disc 1.chd
├── Disc 2.chd
└── Disc 3.chd
```

is represented as one launchable `Final Fantasy VII` entry rather than three independent visible disc entries.

### Variants / duplicates

Distinct ROM files are not automatically considered unwanted duplicates.

Region/revision variants may remain separate entries unless the discovery/metadata model can safely associate them.

Advanced variant grouping may be added later.

### View modes

Baseline view modes:

```text
Grid
List
```

View preference is per-user.

Both modes must preserve access to the required baseline information, including Last Played and Time Played, although the exact visual density may differ.

### Filters

Useful baseline filtering may include:

- system;
- favorites;
- genre when metadata exists;
- players when metadata exists.

The Library does not require a desktop-style advanced query builder.

### Shared versus per-user ownership

```text
SHARED / DEVICE
├── game identity
├── ROM/content references
├── system
├── availability
├── base metadata
├── shared artwork/media
└── discovery state

PER USER
├── favorite
├── hidden state
├── custom collections
├── recently played
├── Last Played
├── Time Played / statistics
├── game-specific preferences/configuration
├── saves/states
└── library view preferences
```

### User Stories

| ID | Priority | User Story |
|---|---|---|
| `US-LIB-001` | MUST | As a user, I want pressing `A` on an available game to launch it immediately. |
| `US-LIB-002` | MUST | As a user, I want a separate controller action to open complete Game Details and management actions. |
| `US-LIB-003` | MUST | As a user, I want Last Played and Time Played visible during normal library browsing without opening Details. |
| `US-LIB-004` | MUST | As a user, I want games automatically organized by system. |
| `US-LIB-005` | MUST | As a user, I want an All Games view across systems. |
| `US-LIB-006` | MUST | As a user, I want personal Favorites and Recently Played views. |
| `US-LIB-007` | MUST | As a user, I want to create personal cross-system custom collections. |
| `US-LIB-008` | MUST | As a user, I want to search the Library by game title. |
| `US-LIB-009` | MUST | As a user, I want Random Game to select from the currently browsed eligible game set. |
| `US-LIB-010` | MUST | As a user, I want missing ROMs/storage to mark known games UNAVAILABLE rather than silently deleting their Library data. |
| `US-LIB-011` | MUST | As a user, I want a returning known ROM/storage to restore the game to AVAILABLE without losing retained data. |
| `US-LIB-012` | MUST | As a user, I want to hide a game from my own Library without deleting its ROM or affecting other users. |
| `US-LIB-013` | MUST | As a user, I want to permanently delete a game and its ROM directly from Game Details/context management with explicit confirmation. |
| `US-LIB-014` | MUST | As a user, I want multi-disc games represented as one logical Library entry when the content relationship is known. |
| `US-LIB-015` | MUST | As a user, I want the Library to remain functional without online metadata services. |
| `US-LIB-016` | MUST | As a user, I want Grid and List browsing modes. |
| `US-LIB-017` | MUST | As a multi-user household, we want shared ROM/metadata storage with independent personal Library state. |

### Acceptance Criteria

| User Story | PASS condition |
|---|---|
| `US-LIB-001` | With an AVAILABLE supported game focused, pressing `A` starts the configured launch flow without requiring a Details screen first. |
| `US-LIB-002` | A documented controller action opens Game Details for the focused entry and exposes applicable metadata/actions. |
| `US-LIB-003` | Normal Grid/List browsing exposes Last Played and cumulative Time Played for played games and a clear neutral state for never-played games. |
| `US-LIB-004` | Supported discovered content is automatically presented under the system identity supplied by EPIC-012. |
| `US-LIB-005` | All Games aggregates eligible entries across systems without duplicating one logical multi-disc game into its component discs. |
| `US-LIB-006` | Favorites and Recently Played differ correctly between two user profiles using the same shared ROM catalog. |
| `US-LIB-007` | A user can create/rename/delete a collection and add/remove cross-system game references without copying/deleting ROMs. |
| `US-LIB-008` | Controller-driven title search returns matching known games and remains usable without a physical keyboard. |
| `US-LIB-009` | Random selection from All Games, a System and a Collection only selects eligible AVAILABLE/non-hidden entries belonging to that current set. |
| `US-LIB-010` | Removing backing removable storage or a ROM causes the known entry to become visibly UNAVAILABLE while preserving metadata and personal state. |
| `US-LIB-011` | Restoring the recognized backing content transitions the same logical entry back to AVAILABLE without resetting retained metadata/favorite/history. |
| `US-LIB-012` | Hiding a game removes it from normal views only for the active user; the ROM remains present and another user remains unaffected. |
| `US-LIB-013` | Delete Game requires explicit destructive confirmation, deletes only content identified as safely owned by that game, reports filesystem failure accurately, and makes the deleted content unavailable to all users. |
| `US-LIB-014` | A validated supported `.m3u` multi-disc set is represented as one logical visible title and launches through its canonical descriptor rather than exposing component discs as ordinary duplicates. |
| `US-LIB-015` | With no network/scraper configuration, newly discovered supported ROMs still appear with locally derived identity and can be launched. |
| `US-LIB-016` | The active user can use both Grid and List modes; switching mode does not alter the underlying Library data. |
| `US-LIB-017` | Two users browsing one shared ROM catalog retain independent favorite/hidden/collection/history/view state. |

### Dependencies

| Dependency | Relationship |
|---|---|
| `EPIC-001` nuubUI | Library browsing and controller interaction |
| `EPIC-003` Multi-user Profiles | per-user Library state |
| `EPIC-004` Settings | Library preferences/hidden-game management |
| `EPIC-006` Notification System | scan/delete/storage-state feedback |
| `EPIC-011` Game Library | this Epic |
| `EPIC-012` Automatic ROM Discovery | game/content identity, system association, availability and deletion ownership |
| `EPIC-015` Game Metadata & ScreenScraper | optional metadata/artwork enrichment |
| play-history/statistics Epic | authoritative Last Played / Time Played tracking |
| emulator/launch Epics | actual launch routing |
| save/state Epics | personal save/state integration |
| achievements Epic | optional achievement detail/action |
| storage Epics | removable-media availability and safe deletion |
| `ENABLER-001` SYSTEM / STATE / USERDATA | shared catalog and per-user persistence |
| `ENABLER-007` Reliability / Failure Handling | safe handling of storage disappearance/deletion failure |

### Platform / architecture requirements

| Area | Required capability |
|---|---|
| Library database/index | Stable logical game identity separate from transient file availability. |
| Shared/user layering | Shared game/content records with per-user favorites, hidden state, collections and history. |
| Availability | Explicit AVAILABLE/UNAVAILABLE state without destructive catalog pruning. |
| Discovery bridge | Consume canonical game/content relationships from EPIC-012. |
| Launch bridge | Immediate launch from focused game. |
| Statistics bridge | Consume Last Played and Time Played. |
| Metadata bridge | Optional enrichment without making metadata mandatory. |
| Safe deletion | Request deletion only through validated content ownership/reference information. |
| Multi-disc model | Represent one logical game with multiple backing files/descriptors. |
| Search/indexing | Fast title lookup suitable for handheld hardware. |
| Random selection | Random eligible entry from current filtered logical result set. |
| Persistence | Atomic storage of shared catalog and per-user state. |

### Current H700 support snapshot

| Capability | Current state | Notes |
|---|---|---|
| Storage/filesystem primitives | `AVAILABLE / H6 QUALIFICATION PENDING` | Final storage/hotplug behavior belongs to platform work. |
| User/profile ownership model | `DEFINED / NOT IMPLEMENTED` | EPIC-003. |
| Game Library database | `NOT IMPLEMENTED` | Userspace product work. |
| ROM discovery bridge | `NOT IMPLEMENTED` | EPIC-012. |
| Last Played / Time Played tracking | `NOT IMPLEMENTED` | Must integrate with launch/session statistics. |
| Metadata integration | `NOT IMPLEMENTED` | EPIC-015. |
| Safe multi-file deletion model | `NOT IMPLEMENTED` | Requires EPIC-012 content ownership model. |
| Random selection | `NOT IMPLEMENTED` | Userspace Library feature. |

### Gaps identified

No kernel change is justified directly by this Epic.

Known userspace/product gaps:

1. define stable logical game/catalog schema;
2. define shared versus per-user database layering;
3. integrate discovery and availability transitions;
4. implement immediate launch action;
5. implement Details action/page;
6. integrate Last Played and Time Played into normal views;
7. implement Favorites/Recent/Collections;
8. implement title search;
9. implement context-aware Random Game;
10. implement hidden-game state;
11. implement AVAILABLE/UNAVAILABLE lifecycle;
12. implement Grid/List views;
13. define safe game/ROM deletion contract with EPIC-012;
14. implement multi-disc logical representation;
15. integrate optional metadata without hard dependency;
16. validate catalog behavior across removable-storage removal/reinsertion.

### Decisions intentionally left open

- exact controller button used for Details;
- exact Random Game presentation after selection;
- exact Grid/List visual layout;
- exact database technology/schema;
- exact policy for retaining/removing orphaned personal saves/states after deliberate ROM deletion;
- advanced region/revision variant grouping;
- exact filters beyond the baseline;
- exact recently-added semantics;
- exact presentation of unavailable titles in each view.

### Refinement result

EPIC-011 is considered **REFINED**.

Approved product decisions include:

- `A` launches immediately;
- another controller action opens complete Game Details;
- Last Played and Time Played are visible in normal Library browsing;
- Random Game is baseline functionality and respects the current browsing/filter context;
- missing content becomes `UNAVAILABLE` rather than being silently forgotten;
- returning recognized content becomes `AVAILABLE` again with retained state;
- Hide Game is per-user and non-destructive;
- Delete Game permanently deletes the backing ROM/content and is device-global/destructive with explicit confirmation;
- multi-file deletion requires validated ownership/reference information;
- multi-disc titles can be represented as one logical game;
- metadata services enhance but are not required for the Library;
- shared ROM/catalog data and per-user organization/history remain separate.

---

## EPIC-012 — Automatic ROM Discovery

**Status:** `REFINED`

### Product intent

nuubOS must automatically discover supported game content from configured storage sources and expose a stable logical game model to `EPIC-011 — Game Library`.

The baseline experience is:

> the user copies ROMs into the expected storage layout and nuubOS discovers and reconciles them automatically.

Discovery must be incremental, non-destructive and storage-aware.

The Game Library does not directly scan filesystems or infer multi-file relationships. EPIC-012 owns filesystem discovery and content relationships.

### Core principles

1. **Automatic discovery**
   Supported ROMs appear without manual catalog entry.

2. **Non-blocking startup**
   nuubUI loads the existing catalog immediately; scanning happens in the background.

3. **Incremental reconciliation**
   A scan updates the existing known catalog rather than rebuilding it destructively.

4. **Storage-aware availability**
   Missing/removable sources transition games to `UNAVAILABLE` rather than deleting catalog state.

5. **Logical game model**
   One logical game may reference one or multiple backing files.

6. **Safe deletion contract**
   Discovery owns the content/reference graph required by EPIC-011 to delete ROMs safely.

7. **Central system registry**
   Supported systems/extensions/default launch metadata are defined centrally rather than duplicated across components.

8. **Efficient scanning**
   No mandatory full hashing of every large ROM during every scan.

### ROM storage layout

The default user-facing layout is organized by system.

Conceptually:

```text
roms/
├── gb/
├── gbc/
├── gba/
├── nes/
├── snes/
├── n64/
├── psx/
├── psp/
├── dreamcast/
├── arcade/
└── ...
```

The exact canonical directory names are defined by the system registry.

### System registry

nuubOS must provide a central machine-readable registry describing each supported system.

At minimum:

```text
system ID
display name
ROM directory name
supported extensions
launch/content rules
multi-file rules
default emulator/core policy reference
BIOS requirements reference
```

The same registry should be consumable by:

- ROM discovery;
- Game Library;
- emulator/core selection;
- BIOS Manager;
- Settings;
- documentation/tooling where appropriate.

System-specific knowledge should not be duplicated unnecessarily.

### Discovery lifecycle

Normal startup:

```text
Boot
  ↓
load existing Library/catalog
  ↓
nuubUI becomes usable
  ↓
incremental ROM scan runs in background
  ↓
catalog reconciled
```

A full blocking scan before nuubUI becomes usable is not acceptable as the normal boot flow.

EPIC-007 provides background-job execution.

EPIC-006 provides Live Notification progress where useful.

### Scan triggers

Baseline scan triggers:

- normal boot/session startup;
- removable-storage insertion;
- explicit manual `Scan for Games`;
- completion of a known nuubOS content import/copy flow.

A permanently active recursive filesystem watcher is **not required** for the baseline product.

### Manual scan

Users must be able to trigger a rescan through a controller-driven product flow.

Conceptually:

```text
Settings
└── Library
    └── Scan for Games
```

An equivalent Library-context action may also exist.

Manual scan uses the same reconciliation engine as automatic scans.

### Storage sources

Discovery must support multiple logical storage sources.

Examples:

```text
USERDATA/internal content storage
SD card
USB/removable storage
future supported storage sources
```

Each source has a stable source identity independent of individual ROM entries where technically possible.

The storage layer may use filesystem UUID, mount/source metadata or another stable source identifier.

### Storage removal

When a known source disappears, the scanner must not delete every associated game record.

Conceptually:

```text
source available
   ↓
games AVAILABLE

source removed
   ↓
source unavailable
   ↓
associated games UNAVAILABLE
```

This transition must preserve Library/user state defined in EPIC-011.

Removal must not crash nuubUI or active background discovery.

### Storage reinsertion

When the known source returns:

```text
source returns
   ↓
reconcile
   ↓
recognized content becomes AVAILABLE
```

Existing logical-game identity and associated retained data should be reused whenever the content relationship can be determined safely.

### Hotplug

Supported removable-media insertion/removal should be handled without reboot.

Insertion may trigger an incremental scan of the new/returned source.

Removal marks affected entries unavailable.

Final hardware hotplug qualification belongs to H6/storage platform work.

### File extension policy

Each system declares a whitelist of supported content extensions.

Examples may include:

```text
PSX
.chd
.cue
.m3u
.pbp

GBA
.gba
.zip        only if supported by policy/backend

SNES
.sfc
.smc
.zip        only if supported by policy/backend
```

Discovery must not blindly treat every file as a possible ROM.

Unknown/unrelated files are ignored by normal discovery.

### Archive policy

Archive handling is system/backend-specific.

Baseline rules:

- `.zip` is recognized only for systems/backends where supported intentionally;
- `.7z` is recognized only if the selected backend/product policy supports it reliably;
- discovery does not perform generic mass extraction;
- scanning should not unpack large archives merely to determine whether they are games.

### Multi-disc content

Multi-disc games are a baseline requirement.

Example:

```text
Final Fantasy VII.m3u
├── Disc 1.chd
├── Disc 2.chd
└── Disc 3.chd
```

Discovery should produce one logical game:

```text
LogicalGame
├── title/identity
├── system
├── canonical launch target: Final Fantasy VII.m3u
└── referenced content
    ├── Disc 1.chd
    ├── Disc 2.chd
    └── Disc 3.chd
```

Component discs referenced by a valid canonical descriptor must not also appear as ordinary independent Library entries unless explicitly required by a supported format/policy.

### CUE/BIN and descriptor/payload sets

Descriptor-based multi-file formats must be represented as one logical game where supported.

Example:

```text
Game.cue
Game (Track 01).bin
Game (Track 02).bin
```

Discovery identifies:

- canonical launch target: `.cue`;
- referenced payload files: `.bin` tracks;
- one logical visible game.

Payload files are not exposed as separate games.

### Logical game model

The discovery output must separate logical game identity from raw files.

Conceptually:

```text
LogicalGame
├── logical identity
├── system
├── source
├── availability
├── canonical launch target
├── primary content
├── referenced content[]
└── content/reference metadata
```

The exact schema is left to implementation.

### Content ownership / reference graph

EPIC-012 must maintain enough relationship data to answer:

> Which files are required by this logical game, and which of those files can be safely deleted?

A backing file may be:

```text
exclusive to one logical game
shared/referenced by multiple logical games
ambiguous/unresolved
```

The delete operation in EPIC-011 must consume this model rather than guessing based on filename patterns.

### Safe deletion contract

For a simple single-file game:

```text
Mario.sfc
```

the discovery layer may identify that file as the complete exclusive backing content.

For a CUE/BIN set:

```text
Game.cue
Track01.bin
Track02.bin
```

the validated owned/reference set may include all files where exclusive.

For `.m3u`:

```text
FF7.m3u
Disc1.chd
Disc2.chd
Disc3.chd
```

the set may include the descriptor and all exclusively referenced discs.

If a file is shared, ambiguous or cannot be proven safe to delete, destructive deletion must not silently remove it.

### Game identity

Filesystem path alone must not be treated as the only long-term game identity.

A path can change while the underlying game remains the same.

However, normal scans must not require expensive complete hashing of all large files.

A layered identity/reconciliation strategy is expected.

Conceptually:

```text
fast discovery identity
├── source identity
├── relative path
├── size
├── timestamps/metadata
└── cached discovery information

optional stronger identity
├── content hash / format identity
└── computed when useful/required and cached
```

The exact implementation is intentionally open.

### Rename / move detection

Detecting that a ROM has been renamed or moved while preserving the same logical identity is `SHOULD`.

If a confident match can be made, existing metadata/user associations should be retained.

If a confident match cannot be made, the safe fallback is:

```text
old entry → UNAVAILABLE
new content → new discovered entry
```

False identity merging is worse than temporary duplication.

### Reconciliation model

A scan must reconcile current storage against the known catalog.

Conceptually:

```text
Known catalog
      +
Current discovered content
      ↓
Reconcile
      ↓
UNCHANGED
ADDED
CHANGED
AVAILABLE_AGAIN
UNAVAILABLE
```

Normal scan must not use a destructive:

```text
delete catalog
rebuild from zero
```

model.

### BIOS separation

ROM discovery and BIOS management are separate concerns.

Conceptually:

```text
roms/ → EPIC-012 Game Discovery
bios/ → BIOS Manager
```

BIOS files must not appear as ordinary games.

BIOS requirements may be referenced from the common system registry.

### Unsupported / unrelated files

Unrecognized files inside ROM directories are ignored during normal discovery.

Examples:

```text
readme.txt
cover.jpg
notes.md
```

The user must not be spammed with warnings for every unrelated file.

Malformed content that otherwise matches a supported format may be logged or surfaced through diagnostics when useful.

### Progress

Large scans run as EPIC-007 jobs.

Where meaningful, progress may expose:

```text
Scanning games
234 / 1,492
```

If the scanner cannot know the total reliably, it must use indeterminate/step-based progress rather than fabricate percentages.

### Performance

Discovery must remain practical with large ROM sets.

Baseline expectations:

- existing Library loads immediately;
- scans run in background;
- incremental/source-specific scans are preferred;
- no systematic full hashing of every ROM on every scan;
- no generic archive extraction during scan;
- scanning must not make foreground UI unusable;
- discovered changes are applied safely and incrementally.

### Shared versus per-user scope

Discovery data is device-global/shared.

EPIC-012 does not own personal:

- favorites;
- hidden state;
- collections;
- Last Played;
- Time Played;
- personal emulator preferences.

Those remain in EPIC-011/related user-scoped features.

### User Stories

| ID | Priority | User Story |
|---|---|---|
| `US-DISC-001` | MUST | As a user, I want supported ROMs copied into the expected locations to be discovered automatically. |
| `US-DISC-002` | MUST | As a user, I want nuubUI to become usable without waiting for a complete ROM scan. |
| `US-DISC-003` | MUST | As a user, I want scanning to reconcile the existing catalog rather than destroy and rebuild my Library state. |
| `US-DISC-004` | MUST | As a user, I want ROMs automatically associated with the correct supported system using a central system registry. |
| `US-DISC-005` | MUST | As a user, I want supported removable storage insertion/removal handled without reboot. |
| `US-DISC-006` | MUST | As a user, I want games from missing storage to become UNAVAILABLE instead of disappearing permanently from the catalog. |
| `US-DISC-007` | MUST | As a user, I want returning known storage/content to restore games to AVAILABLE where identity can be reconciled safely. |
| `US-DISC-008` | MUST | As a user, I want multi-disc `.m3u` games represented as one logical game rather than duplicate disc entries. |
| `US-DISC-009` | MUST | As a user, I want supported descriptor/payload sets such as CUE/BIN represented as one logical game. |
| `US-DISC-010` | MUST | As a user, I want Delete Game to rely on a validated file ownership/reference model so unrelated/shared content is not deleted. |
| `US-DISC-011` | MUST | As a user, I want to trigger a manual Scan for Games. |
| `US-DISC-012` | MUST | As a user, I want unrelated/unsupported files ignored rather than presented as games. |
| `US-DISC-013` | MUST | As a user, I want BIOS files managed separately from ROM discovery. |
| `US-DISC-014` | SHOULD | As a user, I want a confidently detected ROM rename/move to preserve the same logical game identity and retained Library state. |
| `US-DISC-015` | MUST | As a maintainer, I want one central system registry to define system identity, supported extensions and discovery/launch-related metadata. |

### Acceptance Criteria

| User Story | PASS condition |
|---|---|
| `US-DISC-001` | Copying a supported ROM into a configured system ROM location causes it to appear in the shared catalog after an automatic/manual scan without manual database entry. |
| `US-DISC-002` | With an existing catalog and a large ROM set, nuubUI can load/browse the existing Library while a background scan is still running. |
| `US-DISC-003` | A scan updates additions/changes/availability while preserving unaffected logical-game IDs and per-user Library associations. |
| `US-DISC-004` | A representative ROM from each qualified system is classified according to the central registry and exposed with the expected system ID. |
| `US-DISC-005` | Inserting a qualified removable ROM source while running can trigger discovery; removing it does not crash nuubUI and updates availability. |
| `US-DISC-006` | Removing a source containing known games transitions those entries to UNAVAILABLE without deleting their catalog identity. |
| `US-DISC-007` | Reinserting the recognized source and rescanning returns matching known entries to AVAILABLE without resetting retained Library metadata/state. |
| `US-DISC-008` | A validated `.m3u` set produces one logical visible game with the `.m3u` as canonical launch target and suppresses referenced disc entries from normal independent display. |
| `US-DISC-009` | A validated CUE/BIN set produces one logical game using the descriptor as canonical launch target while recording payload dependencies. |
| `US-DISC-010` | The discovery API can return the required file/reference set and whether each file is exclusive/shared/ambiguous; destructive deletion never silently removes an ambiguous/shared file. |
| `US-DISC-011` | A controller-driven manual scan invokes the same reconciliation engine and provides progress/final state without blocking nuubUI for the entire operation. |
| `US-DISC-012` | Representative unrelated files in ROM directories are ignored and do not create Library game entries. |
| `US-DISC-013` | Files in the BIOS storage area are not discovered as ordinary Library games. |
| `US-DISC-014` | Where rename/move recognition is implemented and confidence rules match, the renamed/moved content retains the same logical-game identity; uncertain cases are not falsely merged. |
| `US-DISC-015` | Discovery/system classification rules for supported systems are loaded from one common registry/interface rather than independent hard-coded extension tables in multiple product components. |

### Dependencies

| Dependency | Relationship |
|---|---|
| `EPIC-006` Notification System | discovery progress/result feedback |
| `EPIC-007` Background Jobs & Downloads | background scanning |
| `EPIC-011` Game Library | primary consumer of logical game records and availability |
| BIOS Manager Epic | BIOS separation/requirements |
| emulator/core selection Epics | system registry launch policy |
| Storage/USB/removable-media Epics | source identity, mount, hotplug and removal |
| `ENABLER-001` SYSTEM / STATE / USERDATA | shared catalog persistence/storage layout |
| `ENABLER-002` Multi-device H700 Abstraction | device-specific storage capabilities |
| `ENABLER-003` Boot / Lifecycle Reliability | startup/background discovery lifecycle |
| `ENABLER-004` Hardware Capability Baseline | supported removable storage behavior |
| `ENABLER-006` Logging / Persistent Diagnostics | scan/error diagnostics |
| `ENABLER-007` Reliability / Failure Handling | safe storage-removal/error behavior |

### Platform / architecture requirements

| Area | Required capability |
|---|---|
| System registry | Versioned common registry for supported systems/extensions/content rules. |
| Source manager | Stable representation of internal/removable ROM sources. |
| Reconciliation engine | Incremental comparison of known catalog and current storage. |
| Logical identity | Persistent logical game IDs separate from raw transient paths. |
| Multi-file parser | Safe parsing of supported `.m3u`, `.cue` and other declared descriptor formats. |
| Reference graph | Mapping between logical games and backing files, including shared/exclusive state. |
| Availability tracking | Source/content disappearance does not destroy logical identity. |
| Background execution | Scans execute outside the nuubUI interactive loop. |
| Hotplug integration | Storage insertion/removal events can trigger source state transitions/scans. |
| Safe filesystem access | Malformed files and disappearing storage cannot crash the service/shell. |
| Persistent catalog | Shared discovery state stored atomically in persistent writable storage. |

### Current H700 support snapshot

This is a known-state product snapshot; final H6 qualification is still required.

| Capability | Current state | Notes |
|---|---|---|
| Linux filesystem primitives | `AVAILABLE` | No basic platform blocker. |
| Internal/removable storage architecture | `H6 / TO QUALIFY` | Storage+USB is the next platform milestone area. |
| Hotplug/event primitives | `TO AUDIT/QUALIFY` | Final product behavior depends on H6. |
| Persistent shared catalog | `NOT IMPLEMENTED` | Userspace. |
| System registry | `NOT IMPLEMENTED` | Product architecture. |
| Discovery/reconciliation service | `NOT IMPLEMENTED` | Product/userspace. |
| `.m3u` logical grouping | `NOT IMPLEMENTED` | Userspace. |
| CUE/BIN dependency parsing | `NOT IMPLEMENTED` | Userspace. |
| File reference/ownership graph | `NOT IMPLEMENTED` | Userspace. |
| Rename/move recognition | `NOT IMPLEMENTED` | SHOULD. |

### Gaps identified

No kernel change is justified solely by this Epic at this stage.

Known product/userspace gaps:

1. define the central system registry schema;
2. define canonical ROM directory layout;
3. define storage-source identity contract with H6;
4. implement incremental discovery/reconciliation engine;
5. define stable logical-game identity;
6. implement AVAILABLE/UNAVAILABLE source/content lifecycle;
7. implement manual and automatic scan triggers;
8. integrate storage hotplug events;
9. implement `.m3u` parsing/logical grouping;
10. implement CUE/BIN and other supported descriptor/payload parsing;
11. implement file reference/ownership graph;
12. define safe deletion API consumed by EPIC-011;
13. define archive policy per system/backend;
14. implement scan progress/background-job integration;
15. validate large-romset performance;
16. optionally implement confident rename/move recognition.

### Decisions intentionally left open

- exact canonical ROM folder names;
- exact initial supported system list;
- exact registry serialization format;
- exact persistent catalog/database technology;
- exact source identity implementation;
- exact stronger content-identity/hash strategy;
- exact archive support matrix;
- exact rename/move confidence algorithm;
- exact incremental scan optimization/caching strategy;
- exact handling of exotic multi-file formats beyond the baseline;
- exact behavior when the same physical ROM is intentionally exposed by multiple sources.

### Refinement result

EPIC-012 is considered **REFINED**.

Approved product decisions include:

- automatic ROM discovery from system-organized storage;
- existing Library loads before scanning completes;
- discovery runs as a background job;
- scans reconcile instead of destructively rebuilding the catalog;
- one central system registry defines system/content rules;
- multiple storage sources are supported;
- removable-source loss causes `UNAVAILABLE`, not catalog deletion;
- returning recognized sources/content can restore `AVAILABLE`;
- storage hotplug should not require reboot;
- `.m3u` multi-disc and supported descriptor/payload sets produce one logical game;
- logical game identity is separated from raw file path;
- EPIC-012 owns the content/reference graph used for safe Delete Game;
- shared/ambiguous files are never silently deleted;
- BIOS discovery remains separate;
- unrelated files are ignored;
- systematic full hashing and generic archive extraction are not baseline scan behavior;
- permanent recursive filesystem watching is not required for baseline operation.

---

## EPIC-013 — RetroArch Integration

**Status:** `REFINED`

### Product contract

Provide RetroArch as the default emulation backend where libretro is the best fit, with console-ready defaults and no requirement for normal users to enter the native RetroArch UI.

**MUST**
- preconfigured cores and sane per-system defaults;
- launch/exit controlled by nuubOS;
- clean return to nuubUI;
- per-user RetroArch configuration;
- per-game overrides through EPIC-018;
- save/state integration through EPIC-019;
- controller mapping supplied by the common controller layer;
- Quick Menu integration for supported common actions;
- advanced native RetroArch configuration remains reachable but is not part of normal flow;
- configuration upgrades must preserve user data or migrate safely.

**Product decisions**
- RetroArch is an implementation backend, not the nuubOS shell.
- The supported core/system matrix is explicit and qualified; nuubOS does not ship every possible core merely because it exists.
- Working defaults take priority over exposing every RetroArch option.
- Core binaries are device-global; configuration is per-user where appropriate.

**Acceptance baseline**
A qualified system can be launched from the Library, played with the configured controller, saved, exited and returned to nuubUI without interacting with native RetroArch menus. Two users can retain independent supported configuration.

**Platform/GAP**
Userspace capability; requires final core matrix, configuration-generation/migration layer, lifecycle bridge, controller bridge, Quick Menu bridge and licensing/provenance audit. No kernel change is justified directly by this Epic.

### Refinement result

EPIC-013 is considered **REFINED**. Exact implementation technologies and final per-device qualification remain subject to the relevant platform Enablers and regression matrices.

---

## EPIC-014 — Standalone Emulators

**Status:** `REMOVED` (product decision 2026-10-07)

Removed from the product scope: RetroArch/libretro is the only emulation
backend. The number is kept so other Epic references stay stable.

---

## EPIC-015 — Game Metadata & ScreenScraper

**Status:** `REFINED`

### Product contract

Enrich the shared Game Library with optional metadata/media, initially through ScreenScraper, without making Internet access or scraping mandatory.

**MUST**
- manual scrape for one game and bulk scrape;
- background execution with Live Notification progress;
- title, description, release data, developer/publisher, genre and player count when supplied;
- cover/artwork and screenshots when supplied;
- optional video previews only when enabled/supported;
- match review when confidence is ambiguous;
- never overwrite a correct user-approved match silently;
- shared metadata/media cache device-wide;
- credentials/configuration stored securely;
- Library remains fully usable when scraping is disabled/offline.

**Product decisions**
- ScreenScraper is the initial provider, not the permanent data-model boundary.
- Provider data maps into a provider-neutral nuubOS metadata model.
- Downloaded media is storage-aware and removable/rebuildable without deleting ROMs.
- Metadata language should prefer the active/configured language with sensible fallback where the provider supports it.

**Acceptance baseline**
A discovered ROM can be matched, enriched and displayed without blocking nuubUI; failed/offline scraping leaves the original Library entry intact. Bulk work can be cancelled safely.

**Platform/GAP**
Requires provider adapter, matching engine, cache layout, background jobs, credential storage, rate-limit handling and licensing/terms review for redistributed/cached provider assets.

### Refinement result

EPIC-015 is considered **REFINED**. Exact implementation technologies and final per-device qualification remain subject to the relevant platform Enablers and regression matrices.

---

## EPIC-016 — Game Details

**Status:** `REFINED`

### Product contract

Provide the complete controller-driven information and management view opened by the dedicated Details action defined in EPIC-011.

**MUST**
- show available metadata/artwork;
- show system, availability, Last Played and Time Played;
- show emulator/core currently selected;
- expose Play, Favorite, Collections, Hide and Delete Game;
- expose Game Settings, Saves/States and Achievements only when applicable;
- clearly show `UNAVAILABLE` and relevant storage/source information;
- remain useful with filename-derived metadata only;
- destructive actions follow their owning Epic's confirmation/safety policy.

**Product decisions**
- `A` in the Library still launches immediately; Details is a separate action.
- Details aggregates data owned by other services and does not duplicate their authoritative state.
- Missing metadata produces a clean minimal page, not empty/broken placeholders.

**Acceptance baseline**
An available and an unavailable game can both open Details controller-only; applicable actions route to their canonical feature and metadata absence does not prevent launch/management.

**Platform/GAP**
Primarily nuubUI composition work. Depends on EPIC-011/015/017/018/019/020 and EPIC-012 safe deletion.

### Refinement result

EPIC-016 is considered **REFINED**. Exact implementation technologies and final per-device qualification remain subject to the relevant platform Enablers and regression matrices.

---

## EPIC-017 — RetroAchievements

**Status:** `REFINED`

### Product contract

Provide optional per-user RetroAchievements integration without making an online account necessary for normal emulation.

**MUST**
- credentials/account are per-user;
- enable/disable per user;
- supported games expose achievement status in Details and/or gameplay UX;
- login failure or service outage never blocks launching a game;
- notifications integrate with EPIC-006;
- hardcore mode is supported only where the selected emulator/backend can enforce required semantics correctly;
- logout removes local credentials for that user;
- local bridge architecture is allowed where required by emulator integration.

**Product decisions**
- no device-global shared RetroAchievements identity;
- achievements are optional online functionality;
- nuubOS does not fake achievement unlocks or unsupported compatibility;
- unsupported games simply omit achievement functionality.

**Acceptance baseline**
Two users can use different accounts; one can remain logged out. Supported achievement events are associated with the correct user and offline/service failure degrades gracefully.

**Platform/GAP**
Requires secure credential storage, emulator/core integration or bridge, game identification/hash support and API/service compatibility validation.

### Refinement result

EPIC-017 is considered **REFINED**. Exact implementation technologies and final per-device qualification remain subject to the relevant platform Enablers and regression matrices.

---

## EPIC-018 — Per-game Configuration

**Status:** `REFINED`

### Product contract

Allow user-friendly overrides for individual games while retaining good system defaults.

**MUST**
- configuration is per-user;
- default behavior inherits system/emulator defaults;
- only explicit overrides are stored;
- reset game overrides returns cleanly to inherited defaults;
- candidate common overrides: emulator/core, aspect ratio/video options, performance profile, controller mapping/layout, rumble and supported compatibility options;
- unavailable/unsupported options are hidden or clearly disabled;
- advanced backend-specific options may be exposed separately.

**Product decisions**
- do not copy a full emulator config for every game by default;
- override precedence is explicit: product defaults → system/backend defaults → user defaults → per-game override;
- settings that require restart apply on next launch when necessary.

**Acceptance baseline**
Two users can configure the same ROM differently without affecting each other; resetting one game's overrides restores inherited behavior.

**Platform/GAP**
Requires typed configuration schema, emulator adapters and migration/versioning. No kernel change directly required.

### Refinement result

EPIC-018 is considered **REFINED**. Exact implementation technologies and final per-device qualification remain subject to the relevant platform Enablers and regression matrices.

---

## EPIC-019 — Saves & Save States

**Status:** `REFINED`

### Product contract

Provide reliable, per-user game progress storage across supported emulation backends.

**MUST**
- normal in-game saves and save states are isolated per user;
- multiple state slots where backend supports them;
- save/load through Quick Menu where supported;
- atomic/safe writes where practical;
- no silent deletion when ROM becomes UNAVAILABLE;
- paths are stable across metadata changes and safe ROM rename reconciliation;
- backup/Syncthing can consume user save data;
- incompatible state/backend changes must be communicated rather than blindly loaded.

**SHOULD**
- optional auto-save/auto-load per user/system/game when backend support is reliable;
- state thumbnail/timestamp where cheap.

**Product decisions**
- normal saves are more durable than emulator-specific save states; UI must not imply save-state portability.
- saves/states are not deleted automatically when a ROM is deleted unless the user explicitly chooses such cleanup.
- save ownership follows logical game identity, not display title alone.

**Acceptance baseline**
Two users playing the same ROM create independent saves/states; reboot and ROM temporary unavailability preserve them; failed writes do not report success.

**Platform/GAP**
Requires common save namespace, emulator adapters, atomic persistence, storage-space/error handling and backup/sync integration.

### Refinement result

EPIC-019 is considered **REFINED**. Exact implementation technologies and final per-device qualification remain subject to the relevant platform Enablers and regression matrices.

---

## EPIC-020 — Play Statistics

**Status:** `REFINED`

### Product contract

Track lightweight per-user gameplay history used directly by the Library.

**MUST**
- cumulative Time Played per game;
- Last Played timestamp;
- session count;
- Recently Played ordering;
- statistics are per-user;
- only actual gameplay/application sessions count, not Library browsing;
- abnormal emulator termination must not add unbounded phantom playtime;
- data persists across reboot and temporary ROM unavailability;
- user can reset statistics for a game and, optionally, all personal statistics.

**Product decisions**
- this is local product telemetry, not cloud analytics.
- no global leaderboard or cross-user aggregation is required.
- EPIC-011 consumes Last Played and Time Played directly in normal browsing.

**Acceptance baseline**
Launching and exiting a game updates the correct user's last-played/time/session count; another user's values remain unchanged; interrupted sessions reconcile to a bounded credible duration.

**Platform/GAP**
Requires session lifecycle timestamps, persistent per-user store and clock integration with EPIC-053.

### Refinement result

EPIC-020 is considered **REFINED**. Exact implementation technologies and final per-device qualification remain subject to the relevant platform Enablers and regression matrices.

---

## EPIC-021 — Screenshots & Capture

**Status:** `REFINED`

### Product contract

Provide reliable still screenshots for games and supported applications.

**MUST**
- controller-accessible screenshot action;
- screenshot saved to a predictable per-user location;
- success/failure Toast;
- screenshots survive game exit/reboot;
- no capture of stale/black frames reported as success where the integration can detect failure;
- screenshots are accessible through File Manager and eligible for backup/sync if configured.

**OPTIONAL / DEFERRED**
- video recording;
- streaming/encoding pipeline;
- editing tools.

**Product decisions**
- still screenshots are baseline; video capture is not.
- capture implementation may differ between RetroArch, standalone emulators and other applications behind one product action.

**Acceptance baseline**
A screenshot from each qualified foreground application class produces a valid image associated with the active user and does not materially disrupt gameplay.

**Platform/GAP**
Requires capture adapters/render-path integration and storage handling. Hardware video encoding is not implied.

### Refinement result

EPIC-021 is considered **REFINED**. Exact implementation technologies and final per-device qualification remain subject to the relevant platform Enablers and regression matrices.

---

## EPIC-022 — Controller Management

**Status:** `REFINED`

### Product contract

Provide one coherent controller model for built-in controls and supported external controllers.

**MUST**
- detect and identify controllers;
- controller-first pairing handoff to Bluetooth Manager where applicable;
- per-user mapping/remapping;
- per-user preferred controller and player order/assignment;
- hotplug/disconnect/reconnect handling;
- built-in controls always retain a recovery path;
- rumble/force feedback where hardware/backend supports it;
- controller status visible in Settings;
- no permanent lockout caused by a bad mapping.

**Product decisions**
- physical Bluetooth pairing is device-global; mapping/order/preferences are per-user.
- mappings are translated through a common product abstraction before emulator-specific adapters.
- unsupported rumble is omitted rather than simulated.

**Acceptance baseline**
Built-in controls pass the platform input matrix; a qualified external controller can connect, map, launch/play a game, disconnect/reconnect and retain the correct user's preferences.

**Current H700**
Built-in controls 42/42 PASS; external BLE/UHID/HOG support and 8BitDo Ultimate 2C validation exist; cold-boot reconnect passed. Internal RG35XX Pro rumble is available; external FF remains controller-dependent and final H5 rumble qualification is pending.

**Platform/GAP**
Complete common controller service, mapping schema, recovery mapping and final rumble/FF matrix. USB controller qualification belongs also to H6/EPIC-024.

### Refinement result

EPIC-022 is considered **REFINED**. Exact implementation technologies and final per-device qualification remain subject to the relevant platform Enablers and regression matrices.

---

## EPIC-023 — Keyboard & Mouse

**Status:** `REFINED`

### Product contract

Support keyboard and mouse as optional inputs for workflows that benefit from them without making either necessary for normal console operation.

**MUST**
- USB/Bluetooth keyboard input where platform transport is supported;
- mouse pointer/buttons where applications support them;
- selectable keyboard layout;
- usable in Web Mode, File Manager, streaming and local console/admin contexts;
- plugging/unplugging devices must not break controller navigation;
- virtual keyboard remains available for controller-only use.

**Product decisions**
- nuubUI remains controller-first.
- keyboard/mouse are application/context inputs, not prerequisites for setup or recovery.
- pointer-centric UI is not required for normal shell navigation.

**Acceptance baseline**
A qualified keyboard and mouse can be hotplugged, used in at least Web Mode/application context and removed while controller operation remains functional.

**Platform/GAP**
Keyboard layouts already have `kbd/loadkeys` support. USB/Bluetooth HID qualification and UI/application routing remain.

### Refinement result

EPIC-023 is considered **REFINED**. Exact implementation technologies and final per-device qualification remain subject to the relevant platform Enablers and regression matrices.

---

## EPIC-024 — USB & External Accessories

**Status:** `REFINED`

### Product contract

Define predictable support for useful external USB device classes without promising arbitrary desktop peripheral compatibility.

**MUST**
- USB mass storage;
- qualified game controllers;
- keyboard;
- mouse;
- supported hubs;
- hotplug/remove without shell crash;
- capability/status surfaced to owning feature.

**SHOULD**
- USB audio where validated;
- selected network adapters only if explicitly qualified.

**Product decisions**
- support is class/matrix based, not “all USB devices”.
- unsupported devices fail safely and do not require user-facing kernel details.
- USB power limitations must be respected.

**Acceptance baseline**
Each officially supported class has cold-boot and hotplug regression; storage removal and controller removal do not destabilize nuubUI.

**Platform/GAP**
Direct dependency on H6 Storage+USB and per-board port/OTG/power qualification. No product workaround may use sleep/rebind retry hacks.

### Refinement result

EPIC-024 is considered **REFINED**. Exact implementation technologies and final per-device qualification remain subject to the relevant platform Enablers and regression matrices.

---

## EPIC-025 — Moonlight / PC Game Streaming

**Status:** `REFINED`

### Product contract

Provide Moonlight as the primary low-latency PC streaming client.

**MUST**
- pair/manage compatible hosts;
- browse and launch advertised applications;
- hardware video decode where supported;
- audio playback;
- controller forwarding;
- keyboard/mouse forwarding where applicable;
- clean exit to nuubUI;
- per-user host/configuration credentials;
- selectable practical resolution/FPS profiles;
- network/stream statistics available through Quick Menu where obtainable;
- reconnect/failure behavior is explicit and non-blocking.

**SHOULD**
- rumble forwarding where client/controller stack supports it reliably.

**Product decisions**
- streaming is an application, not native game installation.
- offline host/service failure never affects local gaming.
- unsupported codec/resolution combinations are not exposed as normal choices.

**Acceptance baseline**
A qualified host can pair, stream a game with hardware decode/audio/controller input, exit cleanly and report meaningful stream diagnostics without compromising other users' credentials.

**Platform/GAP**
Requires Moonlight client selection/build, Cedrus/V4L2 decode integration validation, network latency qualification, input/rumble forwarding and licensing review.

### Refinement result

EPIC-025 is considered **REFINED**. Exact implementation technologies and final per-device qualification remain subject to the relevant platform Enablers and regression matrices.

---

## EPIC-026 — Steam Remote Play

**Status:** `REFINED`

### Product contract

Provide Steam Remote Play client capability without implying native Steam/Proton execution on H700.

**MUST**
- discover/connect to configured Steam host through the selected client approach;
- stream video/audio;
- forward controller input;
- clean session exit to nuubUI;
- per-user host/account configuration;
- failure/offline behavior does not affect local console functionality.

**SHOULD**
- keyboard/mouse and rumble forwarding where supported;
- useful stream statistics.

**Product decisions**
- native x86 Steam/Proton gaming is out of scope for this Epic.
- implementation may use an appropriate compatible client rather than reproducing Steam UI.

**Acceptance baseline**
A qualified Steam host can start a remote session, accept controller input and return cleanly to nuubUI.

**Platform/GAP**
Exact client technology, authentication flow, hardware decode and controller-forwarding support require validation.

### Refinement result

EPIC-026 is considered **REFINED**. Exact implementation technologies and final per-device qualification remain subject to the relevant platform Enablers and regression matrices.

---

## EPIC-027 — Network Media / Plex-style Client

**Status:** `REFINED`

### Product contract

Provide optional living-room-style playback from a remote media library.

**MUST**
- browse remote library;
- play supported video and audio;
- subtitles where supported;
- seek/pause/resume;
- remember resume position per user where the backend permits;
- hardware decode for qualified codecs;
- controller-first navigation;
- clean offline/server-unavailable behavior.

**Product decisions**
- “Plex-style” describes the UX; exact backend/protocol is not yet fixed.
- server credentials are per-user when accounts are personal.
- unsupported transcode/direct-play combinations are reported clearly.

**Acceptance baseline**
A representative remote library can be browsed and a qualified video played with audio/subtitles/controller navigation using hardware decode where expected.

**Platform/GAP**
Select backend/client/protocol, validate Cedrus decode formats, subtitle/rendering path, network buffering and credentials.

### Refinement result

EPIC-027 is considered **REFINED**. Exact implementation technologies and final per-device qualification remain subject to the relevant platform Enablers and regression matrices.

---

## EPIC-028 — Local Video & Music Player

**Status:** `REFINED`

### Product contract

Provide simple controller-first playback of local user media.

**MUST**
- browse/play supported video and audio files;
- pause/seek/skip;
- subtitles for supported video formats;
- playlists/queue for music at a basic level;
- hardware decode where qualified;
- remember video resume position per user;
- storage removal/error handled cleanly;
- integrate audio/display routing.

**SHOULD**
- background music playback while browsing nuubUI if lifecycle/audio design supports it cleanly.

**Product decisions**
- this is a lightweight console media player, not a full media-center database.
- codec/container support is an explicit qualified matrix.

**Acceptance baseline**
Representative qualified local media from supported storage plays correctly, survives pause/seek and exits to nuubUI without stale audio/video state.

**Platform/GAP**
Requires media framework/player choice, Cedrus decode validation, subtitle support and audio-route integration.

### Refinement result

EPIC-028 is considered **REFINED**. Exact implementation technologies and final per-device qualification remain subject to the relevant platform Enablers and regression matrices.

---

## EPIC-029 — Wireless Casting to TV

**Status:** `PLANNED AFTER RC` (product decision 2026-10-07: casting is a key feature; every protocol that is technically and legally feasible on H700 is in scope, including the driver work it needs. Order after the RC: Linux 7.3, deep sleep, then this Epic)

### Product contract

Show what the console plays on a TV without a cable. Product forms stay distinct:
- media casting (a video, music or picture is played by the TV/receiver itself);
- remote playback control of that media from the console;
- screen mirroring (the live console screen, including games).

**Protocols in scope** (each one is a separate qualification row; none is advertised before it passes):
- DLNA/UPnP AV (MediaRenderer control point + HTTP media server);
- Google Cast (CASTV2 sender: Default Media Receiver for media; Cast Streaming for mirroring);
- AirPlay (media URL playback to receivers that accept it; mirroring only if it can be done without proprietary keys);
- Miracast (Wi-Fi Direct) and Miracast over Infrastructure (MS-MICE).

**Product decisions**
- AirPlay/Google Cast/Miracast/DLNA are not treated as interchangeable; the UI shows one list of receivers with what each can do.
- wired HDMI/TV Mode remains the reliable baseline external-display path.
- mirroring latency is documented per protocol; games are offered mirroring only where latency is acceptable.
- no proprietary keys, DRM bypass or redistributed SDKs without verified licences.

**Acceptance baseline**
For each protocol: discover a qualified receiver, establish playback/mirroring, control it, recover from disconnect and document codec/network/latency limits.

**Platform/GAP (H700, 2026-10-07)**
- RTL8821CS (rtw88) offers no P2P (Wi-Fi Direct) interface modes: classic Miracast is not possible without driver work; only MS-MICE (over the existing Wi-Fi network) remains.
- Cedrus is decode-only: every mirroring form needs software encoding (H.264/VP8) on the Cortex-A53 cores, competing with the running game.
- AirPlay mirroring uses FairPlay-protected sessions: not implementable with redistributable software.

### Refinement result

EPIC-029 starts after the RC (after Linux 7.3 and deep sleep); protocol order and per-receiver qualification follow the feasibility above.

---

## EPIC-030 — Web Mode

**Status:** `PARKED UNTIL AFTER RC` (product decision 2026-10-07: after Linux 7.3, deep sleep, casting and Spotify Connect (EPIC-056); implementation exists, disabled in the build)

### Product contract

Provide an optional controller-friendly browser/web-app runtime for sites that are technically viable on H700.

**MUST**
- launch/exit as an isolated application;
- controller navigation plus virtual keyboard;
- keyboard/mouse support;
- basic browsing, tabs/history only to the extent needed for a usable lightweight experience;
- per-user browser profile/data;
- hardware-accelerated rendering/video where the selected engine supports it;
- clean recovery from renderer/browser crash;
- no requirement for Web Mode to administer nuubOS itself.

**Product decisions**
- exact browser engine is selected for ARM64 footprint, acceleration and maintainability.
- DRM/Widevine streaming services are not promised unless legally/technically validated.
- Web Mode must not become a desktop environment.

**Acceptance baseline**
A representative standards-based website can be navigated controller-only, text entered, video tested where supported, and browser exited cleanly.

**Platform/GAP**
Browser engine, GPU acceleration, video decode/WebRTC, sandboxing, storage footprint and licensing require qualification.

### Refinement result

EPIC-030 is considered **REFINED**. Exact implementation technologies and final per-device qualification remain subject to the relevant platform Enablers and regression matrices.

---

## EPIC-031 — Local File Manager

**Status:** `REFINED`

### Product contract

Provide a small controller-friendly file manager for user-accessible storage.

**MUST**
- browse;
- copy;
- move;
- rename;
- delete;
- create directories;
- show file size/basic information;
- operate on USERDATA/removable user storage;
- long operations use EPIC-007;
- destructive operations require appropriate confirmation;
- storage disappearance/error does not crash the shell.

**Product decisions**
- SYSTEM and sensitive internal state are hidden/protected from normal File Manager operations.
- this is not a root file explorer.
- controller use is baseline; keyboard/mouse are optional accelerators.

**Acceptance baseline**
Controller-only user can perform common file operations on writable user/removable storage, including a large copy with progress and safe failure handling.

**Platform/GAP**
Requires file-operation service, permissions/path allowlist, background jobs and H6 storage qualification.

### Refinement result

EPIC-031 is considered **REFINED**. Exact implementation technologies and final per-device qualification remain subject to the relevant platform Enablers and regression matrices.

---

## EPIC-032 — Storage Manager

**Status:** `REFINED`

### Product contract

Provide a high-level view of storage devices and nuubOS content locations.

**MUST**
- show known internal/removable storage, capacity and free space;
- show mounted/available state;
- identify configured ROM/media/user-content locations;
- safe eject/unmount where applicable;
- surface read-only/full/error conditions;
- prevent accidental management of immutable SYSTEM as ordinary user storage;
- integrate source availability with Game Library/Discovery.

**SHOULD**
- guided migration of selected user content between supported storage targets.

**Product decisions**
- STATE/SYSTEM internals are not exposed as confusing desktop partitions in normal UX.
- formatting/repartitioning is not baseline unless a later explicit requirement adds it.

**Acceptance baseline**
Insertion/removal of qualified storage updates status correctly; safe eject prevents new writes and completes without corrupting active jobs.

**Platform/GAP**
Directly depends on H6 storage/mount/hotplug architecture and final SYSTEM/STATE/USERDATA layout.

### Refinement result

EPIC-032 is considered **REFINED**. Exact implementation technologies and final per-device qualification remain subject to the relevant platform Enablers and regression matrices.

---

## EPIC-033 — BIOS Manager

**Status:** `REFINED`

### Product contract

Provide a dedicated, shared BIOS/firmware inventory for emulation.

**MUST**
- list BIOS requirements by supported system/backend;
- show present/missing/invalid state;
- validate known BIOS by checksum where the project has a legal checksum reference;
- import/upload BIOS through supported local/Web flows;
- BIOS files are device-global/shared;
- missing BIOS is visible persistently in BIOS Manager and relevant launch/details UX;
- never redistribute copyrighted firmware merely because a checksum is known.

**Product decisions**
- BIOS Manager is separate from ROM discovery.
- multiple acceptable BIOS variants may be modeled explicitly.
- filename alone is insufficient when checksum validation is defined.

**Acceptance baseline**
Known required BIOS can be imported and validated; wrong content is flagged; a system with missing BIOS gets a useful diagnostic without corrupting the Library.

**Current known H700 setup gap**
Previously identified missing files include SGB, PC Engine CD and several PlayStation BIOS variants; final product matrix must be rebuilt from supported emulator requirements rather than blindly copying this development list.

**Platform/GAP**
Requires BIOS registry, hashing/import service, Web integration and strict license/provenance rules.

### Refinement result

EPIC-033 is considered **REFINED**. Exact implementation technologies and final per-device qualification remain subject to the relevant platform Enablers and regression matrices.

---

## EPIC-034 — Syncthing

**Status:** `REFINED`

### Product contract

Integrate Syncthing for optional synchronization of selected per-user data.

**MUST**
- Syncthing is optional and disabled/unconfigured by default;
- configuration/identity is scoped safely per user or through an equivalent isolation model;
- saves and selected user folders can be synchronized;
- sync status/errors are visible in the owning Settings page and may emit Live Notifications;
- conflicts are never silently resolved by deleting one side;
- local gameplay remains functional when peers/network are unavailable;
- service lifecycle does not expose an unauthenticated management UI.

**SHOULD**
- save states, screenshots and selected application data as opt-in folders.

**Product decisions**
- ROM synchronization is not baseline.
- Syncthing is synchronization, not backup.
- user switching must not leak another user's folder/device configuration.

**Acceptance baseline**
Two profiles can have independent sync configuration; a representative save syncs, an offline peer degrades cleanly, and a conflict remains recoverable.

**Platform/GAP**
Requires per-user service/config architecture, credential/UI handling, background status bridge and package/license review.

### Refinement result

EPIC-034 is considered **REFINED**. Exact implementation technologies and final per-device qualification remain subject to the relevant platform Enablers and regression matrices.

---

## EPIC-035 — Backup & Restore

**Status:** `REFINED`

### Product contract

Provide explicit backup and restore independent from Syncthing.

**MUST**
- back up selected user profile/settings, saves, states and statistics;
- create a portable/versioned backup artifact;
- restore validates compatibility/integrity before replacing live data;
- restore is transactional enough to avoid leaving half-restored state;
- user chooses target profile/allowed scope;
- ROMs/BIOS are not included by default;
- backups can be written to supported removable storage.

**SHOULD**
- optional backup of screenshots and selected application data.

**Product decisions**
- backup is user initiated in baseline; scheduled cloud backup is not required.
- restore across nuubOS versions requires explicit migration/compatibility handling.

**Acceptance baseline**
A user can create a backup, change/delete representative personal data, restore it and recover expected settings/save/statistics without affecting another profile.

**Platform/GAP**
Requires backup manifest/version format, atomic restore staging, migration hooks and storage-space validation.

### Refinement result

EPIC-035 is considered **REFINED**. Exact implementation technologies and final per-device qualification remain subject to the relevant platform Enablers and regression matrices.

---

## EPIC-036 — Remote Files & Network Shares

**Status:** `REFINED`

### Product contract

Allow nuubOS to access selected remote filesystems as a client.

**MUST**
- SMB client support;
- credentials stored securely and scoped appropriately;
- browse/mount/unmount through a controller-driven flow;
- connection loss handled without hanging nuubUI;
- remote shares usable by File Manager/media where supported.

**SHOULD**
- NFS for trusted LAN use;
- SFTP client access.

**Product decision**
- **remote ROM execution is not baseline-supported.** ROMs should be imported/copied to qualified local storage unless a future performance/reliability qualification explicitly changes this.
- network shares are optional and never required for core console use.

**Acceptance baseline**
A qualified SMB share can be configured, browsed and copied from; server loss returns a clear unavailable/error state without blocking the shell.

**Platform/GAP**
Requires client packages, mount lifecycle, credential store, timeout/error policy and network-change integration.

### Refinement result

EPIC-036 is considered **REFINED**. Exact implementation technologies and final per-device qualification remain subject to the relevant platform Enablers and regression matrices.

---

## EPIC-037 — Remote Services

**Status:** `REFINED`

### Product contract

Provide individually controllable device-side remote services for administration and file transfer.

**MUST**
- SSH;
- SCP;
- SFTP;
- SMB server;
- each service independently enable/disable from Settings where technically separable;
- disabled by default unless a later onboarding/security decision explicitly changes it;
- no shared hard-coded default password;
- service state persists device-wide;
- exposure limited to appropriate network interfaces/firewall policy;
- service failures do not affect nuubUI.

**Product decisions**
- Dropbear may provide SSH server while OpenSSH client tools remain available if that is the selected footprint.
- remote services are advanced/device-global features.

**Acceptance baseline**
From a LAN client, enabled services authenticate with device credentials and disabled services are not listening/usable.

**Platform/GAP**
Requires final daemon selection/config, service manager, firewall/interface policy, persistent host keys and EPIC-039 credentials.

### Refinement result

EPIC-037 is considered **REFINED**. Exact implementation technologies and final per-device qualification remain subject to the relevant platform Enablers and regression matrices.

---

## EPIC-038 — Web Administration / Library & BIOS Management

**Status:** `REFINED`

### Product contract

Provide a lightweight authenticated Web UI for convenient remote content administration from a phone/PC.

**MUST**
- authentication required;
- show basic device/storage status;
- upload ROMs to supported system locations;
- upload/import BIOS through BIOS Manager validation;
- browse/manage user-accessible library files;
- trigger Library scan;
- show upload/job progress;
- safe Delete Game/content action must use the same EPIC-012 ownership rules as local UI;
- Web UI can be enabled/disabled device-wide.

**Product decisions**
- Web admin is not required for first boot or normal console use.
- it does not expose arbitrary root filesystem editing.
- it reuses product services/APIs instead of implementing a second independent Library/BIOS logic stack.

**Acceptance baseline**
Authenticated LAN browser can upload a ROM, trigger discovery and see it appear in Library; invalid BIOS is rejected/flagged consistently with local BIOS Manager.

**Platform/GAP**
Requires embedded web server/UI, authenticated API, upload staging/limits, CSRF/session security and service exposure policy.

### Refinement result

EPIC-038 is considered **REFINED**. Exact implementation technologies and final per-device qualification remain subject to the relevant platform Enablers and regression matrices.

---

## EPIC-039 — Device Credentials & Service Security

**Status:** `REFINED`

### Product contract

Define secure device-level authentication for remote administration services.

**MUST**
- unique random credential generated per device on first initialization;
- no universal factory/default password;
- credential can be viewed/regenerated/changed through an authenticated local Settings flow;
- SSH host keys persist across normal reboot/update;
- Web admin and remote services use a coherent credential policy;
- secrets stored outside immutable SYSTEM with restrictive permissions;
- regeneration invalidates the old credential predictably;
- SSH public-key authentication is supported.

**SHOULD**
- QR convenience for entering a generated local service URL/credential where it can be done without exposing secrets unnecessarily.

**Product decisions**
- remote-service credentials are device-global, not user-profile passwords.
- local console profiles remain passwordless by default.
- Internet exposure/port forwarding is not a supported default use case.

**Acceptance baseline**
Two freshly initialized devices do not share the same password/host keys; regeneration disables old password authentication; secrets are not world-readable.

**Platform/GAP**
Requires CSPRNG at first boot, persistent STATE paths, password/key management UI and daemon integration.

### Refinement result

EPIC-039 is considered **REFINED**. Exact implementation technologies and final per-device qualification remain subject to the relevant platform Enablers and regression matrices.

---

## EPIC-040 — Wi-Fi Manager

**Status:** `REFINED`

### Product contract

Provide simple controller-first Wi-Fi management.

**MUST**
- scan visible networks;
- connect/disconnect;
- saved networks;
- automatic reconnect;
- hidden SSID entry;
- password entry via virtual/physical keyboard;
- connection/status/IP information;
- forget network;
- failures communicated clearly;
- Wi-Fi configuration device-global;
- offline use remains possible.

**ADVANCED / SHOULD**
- static IPv4/DNS configuration;
- regulatory-domain handling where required by platform;
- network priority if multiple saved networks justify it.

**Product decisions**
- normal UI avoids exposing driver/kernel terminology.
- no Wi-Fi retry/rebind workaround is accepted as a production fix.

**Acceptance baseline**
Connect, reboot/cold boot reconnect, forget and offline flows work controller-only on the qualified H700 Wi-Fi hardware.

**Current H700**
RTL8821CS platform support exists, but H4 is REOPENED because of intermittent severe TX batching/staircase latency. Diagnostic patches 0013–0016 remain uncommitted and firmware provenance for `rtl8821cs_config.bin` must be resolved before redistribution.

**Platform/GAP**
H4 must reach final production qualification before this Epic can be considered platform-supported.

### Refinement result

EPIC-040 is considered **REFINED**. Exact implementation technologies and final per-device qualification remain subject to the relevant platform Enablers and regression matrices.

---

## EPIC-041 — Bluetooth Manager

**Status:** `REFINED`

### Product contract

Provide controller-first management of Bluetooth peripherals.

**MUST**
- scan;
- pair;
- trust;
- connect/disconnect;
- remove;
- show device type/status;
- automatic reconnect for qualified devices;
- device-global physical pairing;
- hand off controller preferences to EPIC-022 and audio routing to EPIC-042;
- failures remain recoverable without terminal use.

**SHOULD**
- battery level where the device/kernel exposes it reliably.

**Product decisions**
- Bluetooth Manager owns physical device relationships; per-user controller mappings remain elsewhere.
- unsupported profiles are not shown as working merely because pairing succeeds.

**Acceptance baseline**
A qualified controller can pair, cold-boot reconnect, disconnect/remove and pair again controller-only; unsupported/failing devices do not destabilize Bluetooth service.

**Current H700**
BLE/UHID/HOG controller support and cold-boot reconnect have passed existing H5 validation. Final matrix expands with audio and remaining FF work.

### Refinement result

EPIC-041 is considered **REFINED**. Exact implementation technologies and final per-device qualification remain subject to the relevant platform Enablers and regression matrices.

---

## EPIC-042 — Bluetooth Audio

**Status:** `REFINED`

### Product contract

Provide Bluetooth audio output as an optional first-class route.

**MUST**
- pair/select supported audio device through Bluetooth/Audio settings;
- connect/reconnect;
- route audio to/from Bluetooth predictably;
- volume/mute integration;
- clear fallback when device disconnects;
- codec/profile shown only where useful;
- gaming latency limitations communicated honestly.

**Product decisions**
- Bluetooth audio is optional; internal/headphone/HDMI routes remain independent.
- nuubOS does not promise low-latency gaming for arbitrary SBC/A2DP devices.
- microphone/headset input is not baseline unless later required.

**Acceptance baseline**
A qualified A2DP device can connect, receive media/game audio, disconnect and fall back to a valid route without reboot.

**Platform/GAP**
Requires BlueZ audio stack choice (e.g. PipeWire/alternative), codec/license assessment, reconnect policy and latency qualification.

### Refinement result

EPIC-042 is considered **REFINED**. Exact implementation technologies and final per-device qualification remain subject to the relevant platform Enablers and regression matrices.

---

## EPIC-043 — Display / TV Mode

**Status:** `REFINED`

### Product contract

Provide coherent rendering on the handheld display and qualified external HDMI/TV modes.

**MUST**
- internal display operation;
- external HDMI detection/output where board supports it;
- supported resolution/refresh selection;
- correct nuubUI scaling;
- safe preview/rollback for display-mode changes;
- orientation/aspect handling;
- hotplug without shell crash;
- external-display audio handoff coordination;
- remember safe user/device preferences.

**Product decisions**
- only qualified modes are exposed.
- a bad display setting must automatically revert.
- simultaneous internal+external behavior is board/capability-specific, not assumed.

**Acceptance baseline**
Each target board passes its display matrix, including cold boot, hotplug, mode switch, rollback and return to handheld-safe output.

**Current H700**
Graphics primitives exist, but the current RG35XX Pro development unit has its LCD physically disconnected, so integrated-display validation must occur on suitable hardware. External display remains to audit.

**Platform/GAP**
Requires per-board DRM/display inventory, EDID/mode policy, UI scaling and audio route integration.

### Refinement result

EPIC-043 is considered **REFINED**. Exact implementation technologies and final per-device qualification remain subject to the relevant platform Enablers and regression matrices.

---

## EPIC-044 — Audio Management

**Status:** `REFINED`

### Product contract

Provide one product-level audio model across supported outputs.

**MUST**
- master volume and mute;
- internal speaker;
- headphone jack where present;
- HDMI audio where supported;
- Bluetooth audio through EPIC-042;
- route status visible;
- sensible automatic route changes on plug/unplug;
- volume changes immediate and controller-accessible;
- persistence of appropriate volume preferences.

**SHOULD**
- USB audio where EPIC-024 qualifies it.

**Product decisions**
- no normal-user ALSA mixer UI.
- hardware-specific mixer quirks stay behind the audio service.

**Acceptance baseline**
Qualified route transitions do not leave duplicate/stuck audio and volume/mute remain coherent across game, media and shell contexts.

**Platform/GAP**
Requires ALSA/audio-server architecture selection, jack/HDMI/Bluetooth event integration and per-board mixer qualification.

### Refinement result

EPIC-044 is considered **REFINED**. Exact implementation technologies and final per-device qualification remain subject to the relevant platform Enablers and regression matrices.

---

## EPIC-045 — Power & Suspend

**Status:** `REFINED`

### Product contract

Provide console-style power lifecycle behavior.

**MUST**
- Power Off;
- Restart;
- Sleep using the qualified suspend mechanism;
- wake from supported inputs;
- power-button behavior;
- application-aware suspend coordination;
- background jobs obey EPIC-007 suspend policy;
- no filesystem corruption from normal suspend/shutdown;
- Power actions remain primary nuubUI actions, not buried in Settings.

**SHOULD**
- configurable display/idle timeout and automatic sleep.

**Product decisions**
- qualified `s2idle` is acceptable; deep STR is not a product requirement by name.
- Restart/Power Off require explicit confirmation; Sleep may be immediate.
- critical OTA/storage phases may temporarily inhibit suspend.

**Acceptance baseline**
Repeated sleep/wake and clean shutdown/reboot cycles preserve input, display, audio, storage and network behavior according to the platform regression matrix.

**Current H700**
s2idle/wake is available; final cross-subsystem regression remains part of platform qualification.

### Refinement result

EPIC-045 is considered **REFINED**. Exact implementation technologies and final per-device qualification remain subject to the relevant platform Enablers and regression matrices.

---

## EPIC-046 — Battery & Charging UX

**Status:** `REFINED`

### Product contract

Provide trustworthy battery and charging status without inventing precision the hardware cannot supply.

**MUST**
- battery percentage where reliable;
- charging/not-charging state;
- low-battery Toast;
- critical-battery protection behavior;
- charger/external-power indication;
- status available in shell/Quick Menu where appropriate;
- no fake time-remaining estimate unless validated.

**SHOULD**
- battery health/temperature only if the power-supply driver exposes meaningful values.

**Product decisions**
- critical battery may force a safe suspend/shutdown flow to protect data.
- thresholds are device-global policy; presentation is product-level.

**Acceptance baseline**
On qualified hardware, unplug/charge/discharge state transitions are correct and critical threshold behavior shuts down/suspends safely without corrupting active writes.

**Current H700**
AXP717/power_supply inventory and physical validation are still required; current RG35XX Pro dev unit has its battery disconnected.

**Platform/GAP**
Requires H7 AXP717 inventory/driver validation and real-battery test hardware.

### Refinement result

EPIC-046 is considered **REFINED**. Exact implementation technologies and final per-device qualification remain subject to the relevant platform Enablers and regression matrices.

---

## EPIC-047 — Performance Profiles

**Status:** `REFINED`

### Product contract

Expose already-developed H700 power/performance controls as simple console profiles.

**MUST**
- `Auto`;
- `Performance`;
- `Battery Saver`;
- current profile visible in Settings/Quick Menu;
- global/device default;
- per-game user override through EPIC-018/Quick Menu;
- thermal safety always overrides user performance requests;
- profile changes do not require reboot.

**Product decisions**
- Auto is the normal default.
- profiles express intent; users do not need raw frequency/governor controls.
- Advanced diagnostics may show actual CPU/GPU frequencies.

**Acceptance baseline**
Each profile applies the expected platform policy, survives required scope persistence, per-game override restores correctly after exit, and thermal limits remain authoritative.

**Current H700**
CPU/GPU DVFS, thermal coupling, Energy Models, `power_allocator` and Auto/Battery Saver infrastructure are FINAL in H7; Performance exposure must use the validated platform controls rather than duplicate them.

### Refinement result

EPIC-047 is considered **REFINED**. Exact implementation technologies and final per-device qualification remain subject to the relevant platform Enablers and regression matrices.

---

## EPIC-048 — OTA Updates

**Status:** `REFINED`

### Product contract

Provide safe system updates while preserving persistent state and user data.

**MUST**
- check update availability;
- show current/target version;
- download in background;
- resume large download where validated;
- cryptographic integrity/authenticity verification;
- explicit install action;
- preserve STATE/USERDATA according to migration contract;
- interrupted/failed update must retain a bootable recovery path;
- update progress via Live Notification;
- release notes/link where available;
- no update requires manual filesystem copying in the normal path.

**Product decisions**
- GitHub Releases may be a distribution backend, but OTA architecture must not trust transport alone.
- automatic unattended installation is not baseline.
- rollback/recovery strategy must be defined before OTA is considered production-ready.

**Acceptance baseline**
A signed/verified test update can be downloaded, installed and rebooted into while preserving representative user data; corrupt/tampered artifact is rejected and interrupted installation recovers safely.

**Platform/GAP**
Requires final partition/image strategy from H6, update manifest/signing, boot/recovery/rollback design, migration hooks and release infrastructure.

### Refinement result

EPIC-048 is considered **REFINED**. Exact implementation technologies and final per-device qualification remain subject to the relevant platform Enablers and regression matrices.

---

## EPIC-049 — Recovery / Factory Reset

**Status:** `REFINED`

### Product contract

Provide explicit recovery/reset operations with clear data-loss boundaries.

**MUST**
- reset one user profile;
- reset system settings;
- repair/recreate damaged mutable configuration where possible;
- full factory reset;
- full reset clearly distinguishes preserving versus erasing USERDATA where architecture permits;
- destructive actions require strong explicit confirmation;
- recovery does not modify immutable SYSTEM unnecessarily;
- reset completion returns to a valid onboarding/boot state.

**Product decisions**
- “factory reset” semantics are documented precisely.
- ROMs/saves are never erased by a vague generic reset prompt.
- recovery operations use the SYSTEM/STATE/USERDATA ownership model.

**Acceptance baseline**
Each reset scope deletes only documented data, leaves excluded scopes intact and produces a bootable deterministic post-reset state.

**Platform/GAP**
Depends strongly on ENABLER-001 final filesystem ownership and recovery boot architecture.

### Refinement result

EPIC-049 is considered **REFINED**. Exact implementation technologies and final per-device qualification remain subject to the relevant platform Enablers and regression matrices.

---

## EPIC-050 — Crash Recovery / Safe Mode

**Status:** `REFINED`

### Product contract

Ensure broken UI/theme/user configuration cannot permanently brick normal access.

**MUST**
- built-in safe/default theme fallback;
- detect repeated nuubUI startup failure;
- enter a minimal safe configuration/path;
- preserve local console/tty recovery for advanced administration;
- allow disabling/resetting the offending user/theme/configuration;
- application crash returns to nuubUI where possible;
- crash loops are bounded and visible, not infinite restart storms.

**Product decisions**
- safe mode prioritizes recovery over custom appearance/services.
- no sleep/retry loop is accepted as a substitute for fixing deterministic platform failures.

**Acceptance baseline**
Deliberately broken theme/config and repeated frontend crash can be recovered without reflashing the device and without deleting unrelated USERDATA.

**Platform/GAP**
Requires shell supervisor/session manager, boot-failure counters/state, safe configuration and tty1 recovery contract.

### Refinement result

EPIC-050 is considered **REFINED**. Exact implementation technologies and final per-device qualification remain subject to the relevant platform Enablers and regression matrices.

---

## EPIC-051 — System Information

**Status:** `REFINED`

### Product contract

Provide a concise controller-friendly device information page.

**MUST**
- nuubOS version/build;
- device/board model;
- kernel version;
- active network/IP summary;
- storage summary;
- battery/charging state where available;
- temperatures where reliable;
- uptime;
- enabled remote services summary.

**SHOULD**
- advanced page with CPU/GPU frequencies, firmware/build identifiers and other support-relevant details.

**Product decisions**
- normal page uses product terminology; raw kernel internals stay Advanced/Diagnostics.
- information is read-only.

**Acceptance baseline**
Displayed identity/version/network/storage values match authoritative system sources on each qualified board and missing hardware data is omitted rather than fabricated.

**Platform/GAP**
Requires common hardware/capability information service and integration with platform telemetry.

### Refinement result

EPIC-051 is considered **REFINED**. Exact implementation technologies and final per-device qualification remain subject to the relevant platform Enablers and regression matrices.

---

## EPIC-052 — Diagnostics & Support Bundle

**Status:** `REFINED`

### Product contract

Provide useful diagnostics for users and maintainers without requiring shell access for first-line support.

**MUST**
- summarize hardware, network, storage, controller, Bluetooth/Wi-Fi and service status;
- expose relevant logs;
- generate an exportable support bundle as a background job;
- include nuubOS/build/board identifiers;
- redact secrets/credentials by default;
- bundle generation failure is non-destructive;
- output is suitable for attaching to a GitHub issue.

**Product decisions**
- support bundle contains diagnostics, not ROMs, BIOS, passwords, Wi-Fi PSKs or service secrets.
- persistent logging remains bounded to protect storage.

**Acceptance baseline**
A generated bundle on a representative failure contains sufficient version/status/log context while automated checks confirm known secret fields are absent.

**Platform/GAP**
Requires ENABLER-006 logging design, redaction rules, bundle manifest and per-subsystem collectors.

### Refinement result

EPIC-052 is considered **REFINED**. Exact implementation technologies and final per-device qualification remain subject to the relevant platform Enablers and regression matrices.

---

## EPIC-053 — Date / Time / RTC

**Status:** `REFINED`

### Product contract

Provide correct device-global time for logs, TLS, saves, synchronization and online services.

**MUST**
- timezone;
- RTC integration where hardware supports it;
- automatic time/NTP enabled by default;
- manual date/time fallback;
- correct persistence/recovery across reboot;
- no onboarding block waiting for network time;
- clear indication when system time is known to be unreliable.

**Product decisions**
- timezone/system clock are device-global.
- localized presentation belongs to EPIC-009.
- network becoming available later should allow time synchronization automatically.

**Acceptance baseline**
Cold boot with/without network produces defined time behavior; NTP correction updates the system cleanly; manual fallback works controller-only; timestamps remain coherent after reboot.

**Platform/GAP**
Requires RTC inventory/qualification on target boards, timezone data and NTP service integration.

### Refinement result

EPIC-053 is considered **REFINED**. Exact implementation technologies and final per-device qualification remain subject to the relevant platform Enablers and regression matrices.

---

## EPIC-054 — Offline Mode

**Status:** `REFINED`

### Product contract

Guarantee that nuubOS remains a useful local console with no Internet connection.

**MUST**
Offline operation retains:
- boot/onboarding with Wi-Fi skipped;
- user selection;
- Game Library;
- local ROM discovery;
- RetroArch/standalone emulation;
- saves/states/statistics;
- local settings;
- local File Manager/media;
- local power/display/audio/controller functions.

Online-only features must:
- fail quickly and clearly;
- never block local game launch;
- retain cached metadata where available;
- expose their offline/unavailable state without notification spam.

**Product decisions**
- no nuubOS cloud account is required.
- Internet connectivity is an enhancement, not a boot dependency.
- LAN-only features may still work while Internet is unavailable.

**Acceptance baseline**
With WAN unavailable from cold boot, the complete local gaming flow remains usable and configured online services degrade independently.

**Platform/GAP**
Cross-Epic regression requirement rather than a new kernel feature.

### Refinement result

EPIC-054 is considered **REFINED**. Exact implementation technologies and final per-device qualification remain subject to the relevant platform Enablers and regression matrices.

---

## EPIC-055 — Privacy & Online Services

**Status:** `REFINED`

### Product contract

Make optional network activity visible and controllable without building a complex privacy dashboard.

**MUST**
- Settings shows configured/enabled optional online services;
- each optional service has a clear enable/configure/logout or equivalent control;
- credentials are stored according to EPIC-039/per-user ownership;
- disabling a service stops its optional network activity except explicit user actions needed to disable/remove it;
- no analytics/telemetry collection is introduced silently;
- core local gaming works with all optional online integrations disabled.

Services include as applicable:
- ScreenScraper;
- RetroAchievements;
- Syncthing;
- OTA update checks;
- remote media;
- Web/remote administration services.

**Product decisions**
- nuubOS has no mandatory cloud account.
- project privacy documentation must describe intentional network-facing/default-enabled behavior.
- service-specific data handling remains owned by that integration.

**Acceptance baseline**
A fresh/offline-oriented configuration can run local gaming with optional integrations disabled; enabling/disabling representative services changes their network behavior predictably.

**Platform/GAP**
Requires a common service/settings inventory, credential ownership model and release-time privacy/network audit.

### Refinement result

EPIC-055 is considered **REFINED**. Exact implementation technologies and final per-device qualification remain subject to the relevant platform Enablers and regression matrices.

---

## EPIC-056 — Spotify Connect

**Status:** `PLANNED AFTER RC` (product decision 2026-10-07: after Linux 7.3, deep sleep and casting, before Web Mode)

### Product contract

Let the console act as a Spotify Connect speaker: a phone/PC Spotify app selects the console as playback device and controls playback, while audio plays through the console's current output.

**MUST**
- the console appears as a Spotify Connect device on the local network while the feature is enabled;
- playback goes through the Product Audio graph (Speaker, Headphones, HDMI, Bluetooth) and follows output changes like every product stream;
- its volume is an application stream volume governed by Product Audio; remote volume from the Spotify app maps to it predictably;
- playback pauses/yields cleanly when a game, stream or other exclusive media session starts, and Home Music does not play on top of it;
- now-playing (track/artist) is shown through a global notification; Quick Menu offers basic control (pause/resume, next, stop) while it is active;
- Sleep/Restart/Power Off go through the central lifecycle path (playback stopped before power actions);
- disabled by default; enabling/disabling is listed in Online Services (EPIC-055);
- credentials/tokens, if stored, are per-user and kept under `secrets/` (EPIC-039).

**Product decisions**
- receiver only: nuubOS does not provide a Spotify browsing/library UI.
- Spotify Premium is a Spotify requirement for Connect playback; the UI says so instead of failing silently.
- no DRM bypass, no caching of protected audio to storage.

**Acceptance baseline**
With the feature on, a phone on the same Wi-Fi selects the console, plays, pauses, skips and changes volume; audio follows Speaker → Headphones → Bluetooth while playing; starting a game stops it; sleep/resume and disabling the feature leave no stale stream or network service.

**Platform/GAP**
- candidate implementation: librespot (Rust, MIT) with its built-in zeroconf (no avahi) and PipeWire/ALSA backend, run as a child of a small nuubOS service that owns enable state, routing and lifecycle;
- librespot is an unofficial client: Spotify's terms for redistributing it in an image and the stability of its authentication (OAuth/zeroconf) must be verified before release;
- CPU/RAM/wakeup cost on H700 and idle behaviour (mDNS responder only while enabled) must be measured.

### Refinement result

EPIC-056 is considered **REFINED** as a product contract; implementation starts after the RC, after EPIC-029 and before EPIC-030.

---

# L. Platform / architecture enablers

The following are not necessarily user-facing Product Epics, but they are required technical enablers for the final nuubOS platform contract.

## ENABLER-001 — SYSTEM / STATE / USERDATA Architecture

**Status:** `TO REFINE`

Keep the immutable/reproducible system separate from persistent system state and user data.

Must eventually define placement and ownership for:

- global configuration;
- user profiles;
- saves;
- states;
- statistics;
- Wi-Fi configuration;
- Bluetooth pairings;
- SSH keys;
- service credentials;
- application state;
- update state;
- logs;
- Syncthing data/configuration.

---

## ENABLER-002 — Multi-device H700 Abstraction

**Status:** `TO REFINE`

Separate common H700 functionality from board-specific hardware support.

Physical validation targets currently available:

- Anbernic RG35XX Pro;
- Anbernic RG CubeXX;
- Anbernic RG40XX-V.

The final capability matrix will track support individually per board.

---

## ENABLER-003 — Boot / Lifecycle Reliability

**Status:** `TO REFINE`

Define and validate:

- cold boot;
- warm reboot;
- shutdown;
- poweroff;
- boot with optional peripherals absent/present;
- deterministic mount/state initialization;
- failure/recovery behavior.

---

## ENABLER-004 — Hardware Capability Baseline

**Status:** `TO REFINE`

Maintain the platform capabilities required by Product Epics across:

- CPU;
- GPU;
- VPU;
- DRM/display;
- audio;
- input;
- rumble;
- Wi-Fi;
- Bluetooth;
- USB;
- storage;
- RTC;
- battery/charging;
- thermal;
- power management.

---

## ENABLER-005 — Security Baseline

**Status:** `TO REFINE`

Define product-wide security requirements.

Candidate scope:

- no default secret credentials;
- secure service exposure;
- filesystem permissions;
- user separation;
- STATE permissions;
- TLS CA bundle;
- update integrity;
- credential lifecycle;
- unnecessary services disabled by default.

---

## ENABLER-006 — Logging & Persistent Diagnostics

**Status:** `TO REFINE`

Define:

- volatile logs;
- persistent crash information;
- support diagnostics;
- log size/rotation;
- privacy implications;
- possible pstore usage.

---

## ENABLER-007 — Reliability / Watchdog / Failure Handling

**Status:** `TO REFINE`

Define expected behavior for:

- hangs;
- kernel panic;
- forced power loss;
- filesystem integrity;
- watchdog strategy;
- crash restart policy.

No mechanism is considered mandatory until product requirements are agreed.

---

## ENABLER-008 — Licensing / Provenance / Redistribution

**Status:** `TO REFINE`

Audit every redistributed component before public release.

Includes:

- Linux;
- kernel patches;
- TF-A;
- U-Boot;
- Buildroot;
- Mesa;
- BlueZ;
- firmware;
- libraries;
- emulators;
- libretro cores;
- fonts;
- themes;
- assets;
- codec libraries;
- browser;
- media components;
- update components.

Track at least:

- license;
- copyright;
- source obligations;
- attribution;
- NOTICE requirements;
- redistribution permission.

---

## ENABLER-009 — Regression & Platform Qualification

**Status:** `TO REFINE`

Define the final regression suite derived from all frozen MUST requirements.

The suite will eventually cover:

- repeated boot/reboot;
- long uptime;
- suspend/resume;
- CPU/GPU/VPU load;
- thermal behavior;
- Wi-Fi;
- Bluetooth;
- controllers;
- rumble;
- storage;
- USB;
- HDMI;
- audio;
- input;
- update/recovery;
- multi-device validation.

---

# M. Open architectural decisions

These are intentionally not resolved yet.

1. Exact nuubUI technology/stack.
2. Exact storage layout and user-profile ownership model.
3. Exact RetroArch core matrix.
4. ~~Which systems require standalone emulators.~~ Resolved 2026-10-07: none (EPIC-014 removed).
5. ScreenScraper account/credential model.
6. RetroAchievements local bridge design.
7. Moonlight integration and target performance profiles.
8. Steam Remote Play client implementation.
9. Plex-style client/backend choice.
10. Exact wireless-casting protocols.
11. Web Mode engine and feature scope.
12. Syncthing process/configuration model for multiple users.
13. Shared versus per-service device credentials.
14. Web administration framework.
15. OTA image/update architecture and rollback.
16. Theme format.
17. Remote network-share support and whether ROMs may run directly from them.
18. Bluetooth audio profile/codec policy.
19. Global versus per-game performance policy.
20. Backup format and restore semantics.
21. Accessibility scope.
22. Privacy/telemetry policy — no telemetry is currently implied by this backlog.
23. Spotify Connect implementation (librespot candidate) and its redistribution terms.

---

# N. Refinement order

The order below is **not frozen**, but is a useful dependency-oriented starting point:

```text
nuubUI
  ↓
Multi-user Profiles
  ↓
SYSTEM / STATE / USERDATA Architecture
  ↓
Settings
  ↓
Game Library
  ↓
Automatic ROM Discovery
  ↓
RetroArch
  ↓
Game Details / Metadata / ScreenScraper
  ↓
Saves / Statistics / RetroAchievements / Syncthing
  ↓
Controller Management / Quick Menu
  ↓
Streaming / Media / Web
  ↓
Storage / Files / Remote Services
  ↓
Connectivity / Display / Audio / Power
  ↓
Updates / Recovery / Diagnostics
  ↓
Security / Licensing / Regression
```

---

# O. Current backlog snapshot

At this stage:

- **56 Product Epics identified**
- **9 Technical Enablers identified**
- **56 Product Epics refined** (`EPIC-001` through `EPIC-056`)
- **0 Epics frozen**
- remaining Epics remain subject to refinement;
- no new platform implementation should be derived from an unrefined Epic until it has been decomposed into explicit requirements.

This document is the working source of truth for Product Epic discovery and refinement.
# M. Practical Development Roadmap

This section is the operational development order for the requirements defined above.

The numbered Product Epics are a requirements taxonomy, **not** the order in which nuubOS should be implemented.

The development strategy is dependency-driven:

```text
platform
   ↓
OS foundations
   ↓
core services / stable product APIs
   ↓
nuubUI + Settings
   ↓
storage/content infrastructure
   ↓
gaming vertical slice
   ↓
gaming UX
   ↓
network/sharing
   ↓
applications/streaming
   ↓
production lifecycle
   ↓
optional features
```

A Product Epic may span multiple milestones. An Epic does not need to be implemented completely in one milestone if only part of its contract is required to unlock the next vertical slice.

The objective of every milestone is a **working, testable system state**, not merely completion of a list of tickets.

---

## P0 — H700 PLATFORM FINAL

### Objective

Finish the hardware/platform foundation before serious product userspace development depends on it.

### Work

```text
H6 Storage + USB
H7 remaining inventory/regression
H5 rumble / force-feedback reopen
H4 Wi-Fi production fix
platform cleanup
full regression
license/provenance audit
```

Existing development rules remain mandatory:

```text
inventory
  ↓
understand hardware
  ↓
compare mainline
  ↓
minimal patch
  ↓
cold boot
  ↓
regression
  ↓
commit
```

Build only through:

```text
./scripts/compile.sh [linux-dirclean]
```

No production fixes based on arbitrary sleep/retry/rebind behavior.

No new commits are allowed before PLATFORM FINAL unless explicitly requested.

### Primary Enablers

- `ENABLER-002` Multi-device H700 abstraction
- `ENABLER-003` Boot / lifecycle reliability
- `ENABLER-004` Hardware capability baseline
- `ENABLER-008` Licensing / provenance / redistribution
- `ENABLER-009` Regression / platform qualification

### Exit criteria

- required H700 subsystems have an explicit qualified state;
- H4 Wi-Fi production issue is resolved or explicitly dispositioned;
- H6 storage/USB baseline is qualified;
- H7 remaining platform work is closed;
- required rumble/FF qualification is complete;
- cold-boot regression passes;
- no known production workaround violates project development rules;
- redistributable platform components have known provenance/license status;
- platform status/documentation reflects reality.

### Deliverable

> **H700 PLATFORM FINAL** — a deterministic, regression-tested hardware platform on which product userspace can be built.

---

## P1 — Foundation

### Objective

Define the durable OS architecture before building feature-rich UI.

### Core architecture

```text
SYSTEM
STATE
USERDATA
ROM storage
BIOS storage
per-user storage
logs
runtime directories
permissions
credentials
```

### Primary Enablers

- `ENABLER-001` SYSTEM / STATE / USERDATA
- `ENABLER-002` Multi-device H700 abstraction
- `ENABLER-003` Boot / lifecycle reliability
- `ENABLER-005` Security baseline
- `ENABLER-006` Logging / persistent diagnostics
- `ENABLER-007` Reliability / watchdog / failure handling
- `ENABLER-008` Licensing / provenance / redistribution

### Product Epics started

- `EPIC-039` Device Credentials & Service Security
- `EPIC-051` System Information
- `EPIC-052` Diagnostics & Support Bundle
- `EPIC-053` Date / Time / RTC
- foundations required later by `EPIC-049` and `EPIC-050`

### Implementation priorities

1. finalize SYSTEM / STATE / USERDATA ownership;
2. define persistent and volatile directory layout;
3. define user-profile storage namespace;
4. define ROM/BIOS/media content roots;
5. define service/runtime directory ownership;
6. establish persistent logging;
7. establish device identity/credential generation;
8. establish system information/capability interface;
9. establish time/RTC/NTP behavior;
10. establish service supervision and deterministic startup;
11. define recovery-safe mutable configuration rules.

### Exit criteria

- SYSTEM is replaceable/reproducible;
- persistent state survives normal SYSTEM replacement;
- USERDATA ownership is explicit;
- secrets are stored with appropriate permissions;
- device has unique credentials/host identity;
- time behavior is deterministic with and without network;
- persistent bounded diagnostics exist;
- boot/service lifecycle is deterministic;
- local console/tty recovery remains possible.

### Deliverable

> nuubOS boots reliably into a structured, persistent and diagnosable operating-system foundation even before the final UI exists.

---

## P2 — Core Services

### Objective

Create stable product services/APIs between nuubUI and Linux/platform internals.

Preferred architecture:

```text
nuubUI
   ↓
nuubOS product services / APIs
   ↓
platform adapters
   ↓
Linux kernel / userspace subsystems
```

Avoid normal product architecture such as:

```text
nuubUI → sysfs directly
nuubUI → bluetoothctl directly
nuubUI → iw directly
nuubUI → mount directly
```

### Product Epics

- `EPIC-003` Multi-user Profiles — backend
- `EPIC-022` Controller Management — backend
- `EPIC-040` Wi-Fi Manager — backend
- `EPIC-041` Bluetooth Manager — backend
- `EPIC-043` Display / TV Mode — backend
- `EPIC-044` Audio Management — backend
- `EPIC-045` Power & Suspend — backend
- `EPIC-046` Battery & Charging UX — backend
- `EPIC-047` Performance Profiles — backend
- `EPIC-051` System Information
- `EPIC-053` Date / Time / RTC

### Core service domains

```text
users
settings
storage
network
bluetooth
controllers
display
audio
power
battery
performance
time
system information
```

### Exit criteria

- product-level interfaces exist for required hardware/settings domains;
- UI code does not need platform-specific shell commands for normal operations;
- state ownership is explicit as device-global or per-user;
- service errors have structured product-level representation;
- interfaces can support additional H700 boards without redesigning nuubUI.

### Deliverable

> A headless/core nuubOS service layer capable of configuring and reporting the fundamental console state through stable product abstractions.

---

## P3 — Shell / Base Console UX

### Objective

Make the console administrable entirely with a controller before gaming functionality is complete.

### Product Epics

- `EPIC-001` nuubUI
- `EPIC-002` First Boot / Onboarding
- `EPIC-003` Multi-user Profiles — UI/session integration
- `EPIC-004` Settings
- `EPIC-006` Notification System
- `EPIC-007` Background Jobs & Downloads
- `EPIC-009` Localization & Regional Settings
- `EPIC-010` Accessibility & Usability / scaling
- `EPIC-051` System Information — UI
- `EPIC-053` Date / Time — UI

### Initial shell target

```text
Home
├── Games
│   └── initially empty / placeholder
├── Applications
│   └── initially minimal
├── Settings
│   └── functional
└── Power
    └── functional
```

### Required flows

```text
boot
 ↓
onboarding if required
 ↓
user/session selection
 ↓
nuubUI
 ↓
Settings / Power
```

The device should be configurable without SSH for normal product settings.

### Exit criteria

- controller-only onboarding works;
- one-user and multi-user session flows work;
- Settings uses the P2 service interfaces;
- Notifications and Live Notifications work;
- background jobs can be represented without blocking the shell;
- correct UI scaling works on qualified display modes;
- Restart / Power Off / Sleep work through nuubUI;
- shell/application lifecycle foundation is stable.

### Deliverable

> A usable controller-first nuubOS console shell with working Settings and system administration, even before the gaming stack is complete.

---

## P4 — Storage & Content Infrastructure

### Objective

Build the content model on which the gaming experience depends.

### Product Epics

- `EPIC-024` USB & External Accessories
- `EPIC-031` Local File Manager
- `EPIC-032` Storage Manager
- `EPIC-033` BIOS Manager
- `EPIC-012` Automatic ROM Discovery
- storage-related portions of `EPIC-011` Game Library

### Core pipeline

```text
Storage Source
     ↓
ROM Discovery
     ↓
Logical Game
     ↓
Content ownership/reference graph
     ↓
Shared Catalog
```

### Required validation

```text
SD insert/remove
USB insert/remove
ROM add/remove
ROM rename/move safe fallback
manual rescan
boot scan
AVAILABLE ↔ UNAVAILABLE
M3U multi-disc
CUE/BIN
unsupported files
BIOS separation
Delete Game ownership query
large ROM-set scan
```

### Exit criteria

- storage sources have stable product identity;
- hotplug/removal does not crash the shell;
- ROM discovery is incremental/non-destructive;
- logical-game catalog persists;
- unavailable storage does not erase Library identity;
- multi-file relationships are represented safely;
- BIOS inventory is independent from ROM discovery;
- safe deletion API can identify exclusive/shared/ambiguous content;
- File Manager and Storage Manager operate only within intended user-accessible scopes.

### Deliverable

> nuubOS can safely understand, inventory and manage game content before an emulator is attached to the complete user flow.

---

## P5 — Gaming MVP

### Objective

Complete one excellent end-to-end gaming vertical slice before broad emulator coverage.

### Product Epics

- `EPIC-011` Game Library
- `EPIC-013` RetroArch Integration
- `EPIC-016` Game Details
- `EPIC-018` Per-game Configuration
- `EPIC-019` Saves & Save States
- `EPIC-020` Play Statistics
- `EPIC-022` Controller Management

### First vertical slice

Start with one simple, well-supported platform such as GBA.

```text
copy ROM
   ↓
automatic discovery
   ↓
Game Library
   ↓
A = immediate launch
   ↓
RetroArch / selected backend
   ↓
play
   ↓
save / state
   ↓
exit
   ↓
return to nuubUI
   ↓
Last Played + Time Played updated
```

Only after this flow is reliable should the support matrix expand.

Suggested progression:

```text
simple 8/16-bit systems
   ↓
GB / GBC / GBA / NES / SNES / Mega Drive / similar
   ↓
PS1
   ↓
more complex systems
   ↓
Dreamcast / PSP / Arcade / others according to qualification
```

### Exit criteria

- Library is controller-usable;
- `A` launches immediately;
- Details action works;
- one system passes complete end-to-end regression;
- save/state ownership is correct per user;
- Last Played / Time Played update correctly;
- game exit always returns to nuubUI;
- controller mapping is coherent;
- unavailable content behaves correctly;
- Delete Game uses safe EPIC-012 ownership information;
- no terminal/native emulator UI is required for normal gameplay.

### Deliverable

> **nuubOS Gaming MVP** — a real console experience from ROM discovery to gameplay and back.

---

## P6 — Gaming UX

### Objective

Turn the functional Gaming MVP into a polished CFW experience.

### Product Epics

- `EPIC-005` Quick Menu / In-game Overlay
- `EPIC-008` Themes
- `EPIC-015` Game Metadata & ScreenScraper
- `EPIC-017` RetroAchievements
- `EPIC-021` Screenshots & Capture
- continued refinement of `EPIC-011`, `EPIC-016`, `EPIC-018`, `EPIC-019`, `EPIC-020`

### Features

```text
Quick Menu
metadata / artwork
ScreenScraper
Random Game
Favorites / Recent / Collections
complete Game Details
themes
achievements
screenshots
statistics overlay
per-game performance/configuration
```

### Exit criteria

- metadata is optional and offline-safe;
- Quick Menu works consistently across qualified backends where capabilities exist;
- theme failure cannot brick nuubUI;
- achievements are isolated per user;
- screenshots are stored correctly;
- Library presentation includes Last Played and Time Played;
- Random Game respects current context and availability;
- normal gaming UX is polished enough for daily use.

### Deliverable

> A complete, pleasant and recognizable nuubOS gaming experience rather than merely a functional emulator launcher.

---

## P7 — Connected / Sharing

### Objective

Add administration, synchronization and LAN sharing using the same core product services.

### Product Epics

- `EPIC-034` Syncthing
- `EPIC-035` Backup & Restore
- `EPIC-036` Remote Files & Network Shares
- `EPIC-037` Remote Services
- `EPIC-038` Web Administration / Library & BIOS Management
- `EPIC-039` Device Credentials & Service Security

### Architecture rule

Local and Web UI must share product logic:

```text
nuubUI ──────┐
             ↓
        nuubOS services/APIs
             ↑
Web UI ──────┘
```

Do not build separate ROM/BIOS/storage logic inside the Web UI.

### Exit criteria

- SSH/SCP/SFTP/SMB service policy is secure and configurable;
- Web UI requires authentication;
- Web ROM upload feeds the same discovery pipeline;
- BIOS upload feeds the same BIOS validation;
- Syncthing does not leak user data across profiles;
- backup/restore is versioned and recoverable;
- SMB/network-share loss does not hang the shell;
- remote ROM execution remains unsupported unless separately qualified.

### Deliverable

> A securely manageable and LAN-connected nuubOS without making network services mandatory for local gaming.

---

## P8 — Applications & Streaming

### Objective

Expand nuubOS beyond local emulation while exercising the mature hardware/service stack.

### Product Epics

- `EPIC-023` Keyboard & Mouse
- `EPIC-025` Moonlight / PC Game Streaming
- `EPIC-026` Steam Remote Play
- `EPIC-027` Network Media / Plex-style Client
- `EPIC-028` Local Video & Music Player
- `EPIC-030` Web Mode
- `EPIC-042` Bluetooth Audio

### Suggested implementation order

```text
Moonlight
   ↓
Steam Remote Play
   ↓
Keyboard / Mouse completion
   ↓
Local Media
   ↓
Network Media
   ↓
Web Mode
   ↓
Bluetooth Audio completion as required
```

Moonlight is a particularly useful integration stress test for:

- Cedrus/VPU;
- Wi-Fi;
- controller forwarding;
- Bluetooth;
- audio;
- display;
- performance profiles;
- suspend/lifecycle.

### Exit criteria

- application lifecycle returns cleanly to nuubUI;
- Moonlight hardware decode/input/audio path is qualified;
- Steam Remote Play path is qualified if technically viable;
- local/network media have an explicit codec matrix;
- Web Mode cannot destabilize the shell;
- per-user application credentials/data remain isolated;
- optional application failures do not affect local gaming.

### Deliverable

> nuubOS becomes a broader handheld/TV entertainment and streaming platform while preserving its console-first core.

---

## P9 — Production Lifecycle

### Objective

Make nuubOS safely maintainable and recoverable as a released product.

### Product Epics

- `EPIC-048` OTA Updates
- `EPIC-049` Recovery / Factory Reset
- `EPIC-050` Crash Recovery / Safe Mode
- `EPIC-052` Diagnostics & Support Bundle — production completion
- `EPIC-054` Offline Mode
- `EPIC-055` Privacy & Online Services

### Important sequencing

Recovery architecture must be considered from P1 onward, but production OTA should be implemented only after SYSTEM/STATE/USERDATA and update ownership are stable.

OTA requires:

```text
versioned artifact
      ↓
download
      ↓
cryptographic verification
      ↓
safe installation
      ↓
reboot
      ↓
health/recovery path
```

### Exit criteria

- update authenticity/integrity is verified;
- interrupted update has a defined recovery path;
- STATE/USERDATA migration is explicit;
- factory-reset scopes are precise;
- broken theme/UI/config can enter Safe Mode;
- support bundle redacts secrets;
- full local gaming works without WAN;
- optional network services are controllable;
- privacy/network behavior is documented;
- release regression and licensing/provenance checks pass.

### Deliverable

> A release-capable nuubOS with safe update, recovery, diagnostics, offline behavior and explicit privacy/network guarantees.

---

## P10 — Optional / Future

### Product Epics

- `EPIC-029` Wireless Casting to TV
- `EPIC-056` Spotify Connect (after EPIC-029, before EPIC-030 Web Mode)
- future product requirements accepted after the baseline release

### Wireless casting gate

EPIC-029 remains deferred until an explicit protocol is selected and validated.

Possible technologies must not be conflated:

```text
media casting
screen mirroring
AirPlay
Google Cast
Miracast
DLNA
```

### Exit criteria for activation

Before moving casting into an active milestone:

- protocol selected;
- licensing/legal feasibility understood;
- H700 encode/decode/network requirements understood;
- receiver compatibility matrix defined;
- representative end-to-end prototype passes.

### Deliverable

> Optional functionality that does not delay the core nuubOS release.

---

# N. Milestone Summary

| Milestone | Primary outcome |
|---|---|
| `P0 — Platform Final` | H700 hardware/platform production baseline |
| `P1 — Foundation` | SYSTEM/STATE/USERDATA, lifecycle, security, logging |
| `P2 — Core Services` | stable product APIs for hardware/settings |
| `P3 — Shell` | nuubUI + onboarding + Settings + Power |
| `P4 — Content` | storage + ROM discovery + BIOS + safe content model |
| `P5 — Gaming MVP` | Library → launch → play → save → exit |
| `P6 — Gaming UX` | Quick Menu, metadata, themes, achievements, screenshots |
| `P7 — Connected` | Web UI, remote services, SMB, Syncthing, backup |
| `P8 — Applications` | Moonlight, Steam Remote Play, media, browser |
| `P9 — Production` | OTA, recovery, diagnostics, privacy, offline qualification |
| `P10 — Optional` | casting and future non-blocking features |

---

# O. Execution Rules

This roadmap is the default implementation order for nuubOS.

For each milestone:

```text
1. inventory current implementation
2. identify Product Epic requirements involved
3. identify Enabler/platform dependencies
4. define the smallest coherent implementation slice
5. implement through stable architecture
6. test normal behavior
7. test failure/recovery behavior
8. cold-boot/regression test where platform-relevant
9. update this document with actual state/gaps
10. only then advance the milestone
```

## Vertical-slice rule

Prefer a complete narrow path over many half-implemented features.

Example:

```text
GOOD

one system
ROM discovery
Library
launch
controller
save
exit
statistics
regression
        ↓
then add systems
```

rather than:

```text
BAD

20 emulators installed
partial Library
partial saves
broken lifecycle
inconsistent controller mappings
```

## Architecture rule

UI components consume product services rather than owning Linux/platform implementation details.

```text
UI / Web UI / Applications
          ↓
nuubOS product services
          ↓
platform adapters
          ↓
Linux / hardware
```

## Product-state rule

The Epic catalog above remains the requirements source.

This roadmap defines **when and in what dependency order** those requirements are implemented.

When implementation reveals a requirement change, update this document before treating the new behavior as the intended product contract.

## Release philosophy

The project advances when a coherent milestone is demonstrably usable and regression-tested.

The objective is not:

> maximize the number of Epics marked implemented.

The objective is:

> progressively build a deterministic, recoverable, console-first nuubOS in vertical slices, without destabilizing already-qualified platform behavior.
