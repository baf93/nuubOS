# nuubOS and the network

nuubOS is a local console: booting, the Game Library, emulation, saves,
settings, Files and every hardware function work without Internet and
without any account. nuubOS collects no analytics or telemetry.

The device talks to the network only for features you turn on or start.
The **Services** section of **Settings → Connectivity** shows Remote Services
and Syncthing with their state and switches; Software Update is in
**Settings → System**, Game Metadata and RetroAchievements are
in **Settings → Gaming**.

| Feature | When it uses the network | What leaves the device | Stored on the device |
|---|---|---|---|
| Wi-Fi time sync (NTP) | when Automatic Time is on and Wi-Fi is connected | time requests | – |
| Game Metadata | only when you ask for metadata (one game, all, or the missing data) | to the source you choose: ScreenScraper — file name, size and checksums of the game, system; TheGamesDB — game name and system; libretro thumbnails — the game's name; a search name you set replaces the file name. ScreenScraper and TheGamesDB are reached through Skyscraper, which identifies itself to them | metadata, images and a request cache in USERDATA; optional ScreenScraper account and TheGamesDB key in STATE (root only), handed to Skyscraper only in memory-backed /run |
| RetroAchievements | after you sign in, while a game runs | your user name and token, game identification and unlocks (sent by RetroArch) | the token only (never the password), per user, in STATE (root only) |
| Software Update | when you check or download in Software Update; with Check Automatically on (default, Settings → System → Software Update) also once a day while Wi-Fi is connected, to look for a new version (never downloads by itself) | requests to SourceForge (GitHub Releases only if SourceForge cannot provide it) for the release manifest of your update channel and, when you download, the update file | the downloaded update in USERDATA until installed |
| Moonlight / Steam Link | only while you use them | the stream with your PC | pairing keys per user |
| Remote Services (SSH, SMB, Web) | off by default; when on, they listen on the local network only | what you transfer | the device credential in STATE (root only) |
| Network Shares (SMB client) | when you connect a share | what you open or copy | share credentials in STATE (root only) |
| Media (Jellyfin) | after you sign in, while you browse or play | requests to your Jellyfin server, playback progress | the server token per user in STATE (root only) |
| Syncthing | only when you enable it for your profile; by default on the local network only (local discovery announces the device ID on the LAN). Sync Away from Home (off by default) uses the public Syncthing discovery and STUN servers to find your devices over the Internet; the data still goes directly between your devices. Allow Relays (off by default, only with Sync Away from Home) lets the end-to-end encrypted data pass through public Syncthing relays when no direct connection works | the folders you turned on (saves, save states, screenshots, covers and metadata, play activity: time played and Recently Played), to the devices you accepted; with Sync Away from Home the device ID and addresses to the discovery servers; with Allow Relays the encrypted traffic, the two device IDs and addresses to a relay. No usage or crash reports, ever | its configuration and keys per user in USERDATA |
| Downloads you start | Steam Link (from Valve, media.steampowered.com), more emulators and the cheat collection (from the libretro buildbot, buildbot.libretro.com), the Miracast encoder (Cisco's OpenH264, ciscobinary.openh264.org), cover thumbnails (thumbnails.libretro.com) | the request for the file | the downloaded files in USERDATA |
| Screen Casting | only while you search for receivers or cast | discovery on the local network (mDNS, SSDP), then the screen and sound to the receiver you chose | settings in STATE |
| Netplay | only while you host or look for a game | LAN broadcast to find hosts, then the game session with the other console on your network; your nuubOS user name as the netplay nickname | – |
| Moonlight Wake PC | when you choose Wake PC | a Wake-on-LAN packet on the local network | the PC's MAC address per user |
| Web (not included in current images) | only while the browser is open | what the sites you open receive; words typed in the address field that are not an address are sent as a search to DuckDuckGo | per user in USERDATA: bookmarks, history, cookies and site data (Clear History / Clear Browsing Data remove them) |

Turning a feature off stops its network activity. Removing an account
(Sign Out) deletes its local credentials. Internet exposure of the device
(port forwarding) is not supported.
