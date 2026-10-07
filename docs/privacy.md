# nuubOS and the network

nuubOS is a local console: booting, the Game Library, emulation, saves,
settings, Files and every hardware function work without Internet and
without any account. nuubOS collects no analytics or telemetry.

The device talks to the network only for features you turn on or start.
**Settings → System → Online Services** lists them with their state.

| Feature | When it uses the network | What leaves the device | Stored on the device |
|---|---|---|---|
| Wi-Fi time sync (NTP) | when Automatic Time is on and Wi-Fi is connected | time requests | – |
| Game Metadata (ScreenScraper) | only when you ask for metadata (one game or all) | file name, size and CRC32 of the game, system | metadata and images in USERDATA; optional ScreenScraper account in STATE (root only) |
| RetroAchievements | after you sign in, while a game runs | your user name and token, game identification and unlocks (sent by RetroArch) | the token only (never the password), per user, in STATE (root only) |
| Software Update | only when you open Software Update and check / download | a request for the release manifest and file | the downloaded update in USERDATA until installed |
| Moonlight / Steam Link | only while you use them | the stream with your PC | pairing keys per user |
| Remote Services (SSH, SMB, Web) | off by default; when on, they listen on the local network only | what you transfer | the device credential in STATE (root only) |
| Network Shares (SMB client) | when you connect a share | what you open or copy | share credentials in STATE (root only) |
| Media (Jellyfin) | after you sign in, while you browse or play | requests to your Jellyfin server, playback progress | the server token per user in STATE (root only) |
| Syncthing | only when you enable it for your profile | the folders you turned on, to the devices you accepted | its configuration and keys per user in USERDATA |
| Web | only while the browser is open | what the sites you open receive; words typed in the address field that are not an address are sent as a search to DuckDuckGo | per user in USERDATA: bookmarks, history, cookies and site data (Clear History / Clear Browsing Data remove them) |

Turning a feature off stops its network activity. Removing an account
(Sign Out) deletes its local credentials. Internet exposure of the device
(port forwarding) is not supported.
