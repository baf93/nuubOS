/* The Web administration API (EPIC-038): topics to read, actions to run.
 *
 * Every topic is a service reply passed on as JSON and every action is
 * one product command with checked arguments; the services keep the
 * rules (what a valid BIOS is, which files Delete Game removes, which
 * settings exist...). Actions that a service does not announce itself are
 * followed by a change event for the other open pages. */

use crate::events::changed;
use crate::files;
use crate::svc::{self, call, call0, kv, sock, sock_ok, sock_timeout, tool, tool_ok};
use serde_json::{json, Map, Value};
use std::fs;
use std::time::Duration;

type R = Result<Value, String>;

/* ------------------------------------------------------------ checks */

fn plain(s: &str, max: usize) -> Result<String, String> {
    if s.len() > max || s.chars().any(|c| c.is_control()) {
        return Err("value".into());
    }
    Ok(s.to_owned())
}

fn token(s: &str) -> Result<String, String> {
    if s.is_empty() || s.len() > 128 || !s.chars().all(|c| c.is_ascii_alphanumeric() || "._:@+-/".contains(c)) || s.contains("..") {
        return Err("value".into());
    }
    Ok(s.to_owned())
}

fn game_id(s: &str) -> Result<String, String> {
    if s.len() == 16 && s.chars().all(|c| c.is_ascii_hexdigit()) {
        Ok(s.to_owned())
    } else {
        Err("game".into())
    }
}

fn int(s: &str, lo: i64, hi: i64) -> Result<i64, String> {
    s.trim().parse::<i64>().ok().filter(|v| (lo..=hi).contains(v)).ok_or_else(|| "value".into())
}

fn one_of(s: &str, list: &[&str]) -> Result<String, String> {
    if list.contains(&s) {
        Ok(s.to_owned())
    } else {
        Err("value".into())
    }
}

/* Game/System Settings keys: the settings, "remap" (Reset Controls) and
 * remap_<control> (Controls). */
fn settings_key(s: &str) -> Result<String, String> {
    const CONTROLS: [&str; 16] = ["menu_confirm", "menu_back", "face_north", "face_west", "menu_up", "menu_down",
        "menu_left", "menu_right", "l1", "r1", "l2", "r2", "l3", "r3", "settings", "select"];
    if s.strip_prefix("remap_").is_some_and(|c| CONTROLS.contains(&c)) {
        return Ok(s.to_owned());
    }
    one_of(s, &["core", "aspect", "scaling", "filter", "accuracy", "pak", "resolution", "frameskip", "remap"])
}

fn flag(s: &str) -> Result<&'static str, String> {
    match s {
        "1" | "true" => Ok("1"),
        "0" | "false" => Ok("0"),
        _ => Err("value".into()),
    }
}

fn arg(b: &Value, k: &str) -> String {
    match b.get(k) {
        Some(Value::String(s)) => s.clone(),
        Some(Value::Number(n)) => n.to_string(),
        Some(Value::Bool(v)) => if *v { "1".into() } else { "0".into() },
        _ => String::new(),
    }
}

fn user() -> Result<String, String> {
    let u = svc::active_user();
    if u.is_empty() { Err("no-user".into()) } else { Ok(u) }
}

/* ------------------------------------------------------------ topics */

fn read_kv_file(path: &str) -> Value {
    kv(&fs::read_to_string(path).unwrap_or_default())
}

fn merge(a: &mut Value, b: Value, prefix: &str) {
    if let (Value::Object(a), Value::Object(b)) = (a, b) {
        for (k, v) in b {
            a.insert(format!("{prefix}{k}"), v);
        }
    }
}

/* A catalog name: "it", or a regional code such as "pt-br", "es-419",
 * "zh-cn"; anything else is English. */
fn catalog_lang(lang: &str) -> &str {
    let (base, region) = lang.split_once('-').unwrap_or((lang, "ok"));
    let valid = base.len() == 2 && base.chars().all(|c| c.is_ascii_lowercase())
        && (2..=3).contains(&region.len()) && region.chars().all(|c| c.is_ascii_lowercase() || c.is_ascii_digit());
    if valid { lang } else { "en" }
}

pub fn catalog(lang: &str) -> Vec<String> {
    let lang = catalog_lang(lang);
    let text = fs::read_to_string(format!("{}/usr/share/nuubos/i18n/{lang}.lang", crate::root())).unwrap_or_default();
    let mut out: Vec<String> = Vec::new();
    for line in text.lines() {
        let Some((k, v)) = line.split_once('=') else { continue };
        let Ok(i) = k.parse::<usize>() else { continue };
        if i >= 4096 {
            continue;
        }
        if out.len() <= i {
            out.resize(i + 1, String::new());
        }
        out[i] = v.replace("\\n", "\n");
    }
    out
}

pub fn language() -> String {
    let v = kv(&sock("localizationd", "STATUS").unwrap_or_default());
    v["language"].as_str().unwrap_or("en").to_owned()
}

pub fn mood() -> Value {
    let v = kv(&tool_ok("nuubos-themectl", &["tokens"]).unwrap_or_default());
    let mut m = Map::new();
    if let Value::Object(o) = v {
        for (k, val) in o {
            if let Some(s) = val.as_str() {
                let color = s.len() == 7 && s.starts_with('#') && s[1..].chars().all(|c| c.is_ascii_hexdigit());
                if color || k == "id" {
                    m.insert(k, Value::String(s.to_owned()));
                }
            }
        }
    }
    Value::Object(m)
}

pub fn topic(t: &str, q: &dyn Fn(&str) -> String) -> R {
    Ok(match t {
        "status" => read_kv_file(&format!("{}/statusd.state", svc::run_dir())),
        "system" => kv(&sock("systemd", "STATUS")?),
        "audio" => kv(&sock("audiod", "STATUS")?),
        "users" => kv(&sock("usersd", "STATUS")?),
        "localization" => kv(&sock("localizationd", "STATUS")?),
        "regional" => kv(&sock("regionald", "STATUS")?),
        "library" => kv(&sock("libraryd", "STATUS")?),
        "jobs" => kv(&sock("jobd", "STATUS")?),
        "emu" => kv(&sock("emud", "STATUS")?),
        "emu_settings" => kv(&sock("emud", "SETTINGS")?),
        "timezones" => kv(&sock("regionald", "LIST_TIMEZONES")?),
        "keyboards" => kv(&sock("regionald", "LIST_KEYBOARDS")?),
        "filters" => kv(&sock("libraryd", "FILTERS")?),
        /* The system registry: every supported system with its ROM folder
         * and file types (upload targets, also systems without games). */
        "systems" => {
            let text = fs::read_to_string(format!("{}/usr/share/nuubos/systems.conf", crate::root())).unwrap_or_default();
            let list: Vec<Value> = text
                .lines()
                .filter(|l| !l.trim().is_empty() && !l.starts_with('#'))
                .filter_map(|l| {
                    let f: Vec<&str> = l.split('|').collect();
                    (f.len() >= 6).then(|| json!({ "id": f[0], "name": f[1], "folder": f[2], "extensions": f[3], "color": f[5],
                        "aliases": f.get(6).copied().unwrap_or("") }))
                })
                .collect();
            json!({ "systems": list })
        }
        "remote" => kv(&tool_ok("nuubos-remotectl", &["status"])?),
        "password" => json!({ "password": tool_ok("nuubos-remotectl", &["credential", "show"])?.trim() }),
        /* SSH public keys (EPIC-039): key = fingerprint, type, comment. */
        "ssh_keys" => kv(&tool_ok("nuubos-remotectl", &["keys", "list"])?),
        "sync" => kv(&sock("syncd", "STATUS")?),
        "tailscale" => kv(&sock("tsd", "STATUS")?),
        "scraper" => kv(&tool_ok("nuubos-scraper", &["account", "status"])?),
        "bios" => kv(&tool_ok("nuubos-biosctl", &["status"])?),
        "storage" => kv(&tool_ok("nuubos-storagectl", &["status"]).unwrap_or_default()),
        "media" => kv(&tool_ok("nuubos-mediactl", &["list"])?),
        "shares" => kv(&tool_ok("nuubos-sharesctl", &["list"])?),
        "recovery" => kv(&tool_ok("nuubos-recoveryctl", &["safe-mode", "status"]).unwrap_or_default()),
        "roots" => kv(&tool_ok("nuubos-filesctl", &["roots"])?),
        "update" => {
            let mut v = kv(&tool_ok("nuubos-updatectl", &["status"])?);
            merge(&mut v, kv(&tool_ok("nuubos-update-apply", &["status"]).unwrap_or_default()), "apply_");
            v
        }
        "ra" => match svc::active_user() {
            u if u.is_empty() => json!({}),
            u => kv(&tool_ok("nuubos-achievementsctl", &["status", &u])?),
        },
        "themes" => {
            let u = svc::active_user();
            let list = kv(&tool_ok("nuubos-themectl", &["list"])?);
            let sel = if u.is_empty() { json!({}) } else { kv(&tool_ok("nuubos-themectl", &["selected", &u]).unwrap_or_default()) };
            json!({ "list": list["theme"], "selected": sel["id"], "tokens": mood() })
        }
        "display" => {
            let d = svc::bin("nuubos-displayctl");
            let b = tool(&d, &["brightness"], None).unwrap_or_default();
            let hdmi = tool(&d, &["hdmi"], None).unwrap_or_default();
            let temp_ok = tool(&d, &["temperature-supported"], None).is_ok();
            let temp = tool(&d, &["temperature"], None).unwrap_or_default();
            json!({ "brightness": b.trim(), "hdmi": hdmi.trim() == "connected", "temperature_supported": temp_ok, "temperature": temp.trim() })
        }
        "wifi" => {
            let (enabled, state, ssid, ipv4, signal): (bool, String, String, String, i32) = call("Wifi", "GetSnapshot", &())?;
            let nets: Vec<(String, String, i32, bool, bool)> = call("Wifi", "GetNetworks", &()).unwrap_or_default();
            let saved: Vec<(String, String, i32, bool, bool)> = call("Wifi", "GetSavedNetworks", &()).unwrap_or_default();
            let wps: bool = call("Wifi", "GetWpsActive", &()).unwrap_or(false);
            let row = |n: &(String, String, i32, bool, bool)| json!({ "ssid": n.0, "security": n.1, "signal": n.2, "saved": n.3, "current": n.4 });
            json!({ "enabled": enabled, "state": state, "ssid": ssid, "ipv4": ipv4, "signal": signal, "wps": wps,
                    "networks": nets.iter().map(row).collect::<Vec<_>>(), "saved": saved.iter().map(row).collect::<Vec<_>>() })
        }
        "wifi_ip" => {
            let ssid = plain(&q("ssid"), 64)?;
            let r: (String, String, u32, String, String, String, String, u32, String, String, String) =
                call("Wifi", "GetIpConfiguration", &(ssid.as_str(),))?;
            json!({ "mode": r.0, "address": r.1, "prefix": r.2, "netmask": r.3, "gateway": r.4, "dns": r.5,
                    "runtime_address": r.6, "runtime_prefix": r.7, "runtime_netmask": r.8, "runtime_gateway": r.9, "runtime_dns": r.10 })
        }
        "bluetooth" => {
            let (present, powered, address, connected, paired): (bool, bool, String, u32, u32) = call("Bluetooth", "GetSnapshot", &())?;
            let devs: Vec<(String, String, String, bool, bool, bool, i32, i32)> = call("Bluetooth", "GetDevices", &()).unwrap_or_default();
            json!({ "present": present, "powered": powered, "address": address, "connected": connected, "paired": paired,
                    "devices": devs.iter().map(|d| json!({ "address": d.0, "name": d.1, "kind": d.2, "paired": d.3,
                        "connected": d.4, "trusted": d.5, "rssi": d.6, "battery": d.7 })).collect::<Vec<_>>() })
        }
        "controllers" => {
            let devs: Vec<(String, String, String, bool, bool, i32, i32, i32)> = call("Controllers", "GetDevices", &())?;
            let asg: Vec<(i32, String, String, bool)> = call("Controllers", "GetAssignments", &()).unwrap_or_default();
            json!({ "devices": devs.iter().map(|d| json!({ "id": d.0, "name": d.1, "kind": d.2, "connected": d.3,
                        "builtin": d.4, "player": d.6, "battery": d.7 })).collect::<Vec<_>>(),
                    "players": asg.iter().map(|a| json!({ "player": a.0, "id": a.1, "name": a.2, "available": a.3 })).collect::<Vec<_>>() })
        }
        "deadzones" => {
            let id = plain(&q("id"), 128)?;
            let (l, r): (i32, i32) = call("Controllers", "GetDeadzones", &(id.as_str(),))?;
            json!({ "left": l, "right": r })
        }
        "lighting" => {
            let s: (bool, String, String, i32, String, u32, String) = call("Lighting", "GetState", &())?;
            json!({ "supported": s.0, "effect": s.1, "color": s.2, "brightness": s.3, "speed": s.4, "signals": s.5, "active_signal": s.6 })
        }
        "rumble" => {
            let (supported, enabled): (bool, bool) = call("Rumble", "GetState", &())?;
            let mode: String = call("Rumble", "GetMode", &())?;
            json!({ "supported": supported, "enabled": enabled, "mode": mode })
        }
        "avatars" => kv(&sock("usersd", &format!("LIST_AVATARS\t{}", token(&q("user"))?))?),
        "games" => {
            let scope = q("scope");
            let ok = scope == "recent" || scope.strip_prefix("system:").or(scope.strip_prefix("collection:")).map(|v| token(v).is_ok()).unwrap_or(false);
            if !ok {
                return Err("scope".into());
            }
            kv(&sock_timeout("libraryd", &format!("GAMES\t{scope}"), Duration::from_secs(8))?)
        }
        "search" => kv(&sock_timeout("libraryd", &format!("SEARCH\t{}", plain(&q("spec"), 512)?), Duration::from_secs(8))?),
        "details" => kv(&sock("libraryd", &format!("DETAILS\t{}", game_id(&q("id"))?))?),
        "game_settings" => kv(&sock("emud", &format!("GAME_SETTINGS\t{}\t{}", game_id(&q("id"))?, token(&q("system"))?))?),
        /* System Settings (GitHub #12): what every game of a system inherits. */
        "system_settings" => kv(&sock("emud", &format!("SYSTEM_SETTINGS\t{}", token(&q("system"))?))?),
        "files" => {
            let p = files::user_path(&q("path")).ok_or("path")?;
            kv(&tool_ok("nuubos-filesctl", &["list", &p])?)
        }
        _ => return Err("topic".into()),
    })
}

/* ----------------------------------------------------------- actions */

/* Something only this page changed: tell the others. */
fn done(topic: &str) -> R {
    changed(topic);
    Ok(json!({ "ok": true }))
}

fn ok() -> R {
    Ok(json!({ "ok": true }))
}

/* Work that ends this daemon or the system: answer first. */
fn later(bin: &str, args: Vec<String>) -> R {
    let bin = svc::bin(bin);
    std::thread::spawn(move || {
        std::thread::sleep(Duration::from_millis(600));
        let _ = std::process::Command::new("setsid").arg(bin).args(args).spawn();
    });
    ok()
}

fn job(kind: &str, args: &[&str]) -> R {
    let mut cmd = format!("SUBMIT\t{kind}");
    for a in args {
        cmd.push('\t');
        cmd.push_str(&plain(a, 1024)?);
    }
    let out = sock("jobd", &cmd)?;
    svc::reply_ok(&out)?;
    let id = out.split_whitespace().nth(1).unwrap_or("").to_owned();
    Ok(json!({ "ok": true, "job": id }))
}

fn emu_key(k: &str) -> Result<String, String> {
    let fixed = ["boot_screens", "slots", "slot_rotation", "bezels", "crt", "position", "overlay", "overlay_items", "retroarch_menu"];
    if fixed.contains(&k) || (k.starts_with("hotkey_") && token(k).is_ok()) {
        Ok(k.to_owned())
    } else {
        Err("key".into())
    }
}

pub fn action(b: &Value) -> R {
    let op = arg(b, "op");
    let a = |k: &str| arg(b, k);
    match op.as_str() {
        /* Power and system (nuubos-systemd). */
        "power.sleep" => { sock_ok("systemd", "ACTION SLEEP")?; ok() }
        "power.restart" => { sock_ok("systemd", "ACTION RESTART")?; ok() }
        "power.poweroff" => { sock_ok("systemd", "ACTION POWEROFF")?; ok() }
        "system.profile" => { sock_ok("systemd", &format!("SET PROFILE {}", one_of(&a("v"), &["auto", "battery-saver"])?))?; ok() }
        "system.autobattery" => { sock_ok("systemd", &format!("SET AUTO_BATTERY {}", int(&a("v"), 0, 50)?))?; ok() }
        "system.chargelimit" => { sock_ok("systemd", &format!("SET CHARGE_LIMIT {}", int(&a("v"), 80, 100)?))?; ok() }
        "system.sleep" => { sock_ok("systemd", &format!("SET SLEEP {}", int(&a("v"), 0, 240)?))?; ok() }
        "system.hypersleep" => { sock_ok("systemd", &format!("SET HYPERSLEEP {}", one_of(&a("v"), &["0", "1"])?))?; ok() }
        "system.faststartup" => { sock_ok("systemd", &format!("SET FAST_STARTUP {}", one_of(&a("v"), &["0", "1"])?))?; ok() }
        "system.powerkeysleep" => { sock_ok("systemd", &format!("SET POWER_KEY_SLEEP {}", one_of(&a("v"), &["0", "1"])?))?; ok() }
        "system.lidwake" => { sock_ok("systemd", &format!("SET LID_WAKE {}", one_of(&a("v"), &["0", "1"])?))?; ok() }
        /* The games card (SD2) and the console backup. */
        "storage.eject" => { sock_ok("systemd", "STORAGE SD2 EJECT")?; ok() }
        "storage.use" => { sock_ok("systemd", "STORAGE SD2 USE")?; ok() }
        "storage.backup.enabled" => { sock_ok("systemd", &format!("SET STORAGE BACKUP {}", one_of(&a("v"), &["0", "1"])?))?; ok() }
        "storage.backup.item" => {
            let item = one_of(&a("item"), &["saves", "states", "library", "screenshots", "bios", "covers"])?;
            sock_ok("systemd", &format!("SET STORAGE BACKUP_ITEM {} {}", item, one_of(&a("v"), &["0", "1"])?))?;
            ok()
        }
        "storage.backup.dest" => { sock_ok("systemd", &format!("SET STORAGE BACKUP_DEST {}", one_of(&a("v"), &["sd2", "sd1", "usb"])?))?; ok() }
        "storage.backup" => job("sd2-backup", &[]),
        "storage.prepare" => job("sd2-prepare", &[]),
        "storage.format" => job("sd2-format", &[&token(&a("cid"))?]),

        /* Display (nuubos-displayd). */
        "display.brightness" => { tool_ok("nuubos-displayctl", &["brightness", &int(&a("v"), 1, 100)?.to_string()])?; done("display") }
        "display.temperature" => { tool_ok("nuubos-displayctl", &["temperature", &int(&a("v"), 3500, 7500)?.to_string()])?; done("display") }

        /* Audio (nuubos-audiod). */
        "audio.output" => { call0("Audio", "SetOutput", &(one_of(&a("v"), &["auto", "analog", "bluetooth", "usb", "hdmi"])?.as_str(),))?; ok() }
        "audio.volume" => { call0("Audio", "SetVolume", &(int(&a("v"), 0, 100)? as i32,))?; ok() }
        "audio.system" => { sock_ok("audiod", &format!("SYSTEM VOLUME SET {}", int(&a("v"), 0, 100)?))?; done("audio") }
        "audio.apps" => { sock_ok("audiod", &format!("APPLICATIONS VOLUME SET {}", int(&a("v"), 0, 100)?))?; done("audio") }
        "audio.music" => { sock_ok("audiod", &format!("MUSIC VOLUME SET {}", int(&a("v"), 0, 100)?))?; done("audio") }
        "audio.step" => { sock_ok("audiod", &format!("VOLUME STEP SET {}", one_of(&a("v"), &["1", "2", "5", "10"])?))?; done("audio") }
        "audio.musicon" => { sock_ok("audiod", &format!("MUSIC ENABLED SET {}", flag(&a("v"))?))?; done("audio") }
        "audio.nav" => { sock_ok("audiod", &format!("SYSTEM NAVIGATION SET {}", flag(&a("v"))?))?; done("audio") }
        "audio.notify" => { sock_ok("audiod", &format!("SYSTEM NOTIFY SET {}", flag(&a("v"))?))?; done("audio") }
        "audio.power" => { sock_ok("audiod", &format!("SYSTEM POWER SET {}", flag(&a("v"))?))?; done("audio") }
        "audio.musicplay" => { sock_ok("audiod", &format!("MUSIC {}", one_of(&a("v"), &["START", "STOP", "NEXT"])?))?; done("audio") }
        "audio.test" => { sock_ok("audiod", &format!("TEST {}", one_of(&a("v"), &["START", "STOP"])?))?; done("audio") }

        /* Language, time, keyboard. */
        "lang.set" => { sock_ok("localizationd", &format!("SET LANGUAGE {}", one_of(&a("v"), &["en", "it", "fr", "de", "es", "es-419", "pt", "pt-br", "nl", "pl", "cs", "sv", "da", "nb", "fi", "hu", "ro", "tr", "el", "ru", "uk", "id", "vi", "th", "hi", "ja", "zh-cn", "zh-tw", "ko"])?))?; ok() }
        "tz.set" => { sock_ok("regionald", &format!("SET_TIMEZONE\t{}", token(&a("v"))?))?; ok() }
        "time.auto" => { sock_ok("regionald", &format!("SET_AUTOMATIC_TIME\t{}", flag(&a("v"))?))?; ok() }
        "time.set" => {
            let v = a("v");
            let good = v.len() == 16 && v.chars().enumerate().all(|(i, c)| match i { 4 | 7 => c == '-', 10 => c == ' ', 13 => c == ':', _ => c.is_ascii_digit() });
            if !good { return Err("value".into()); }
            sock_ok("regionald", &format!("SET_LOCAL_TIME\t{v}"))?; ok()
        }
        "time.sync" => { svc::reply_ok(&sock_timeout("regionald", "SYNC_NOW", Duration::from_secs(15))?)?; ok() }
        "kbd.set" => { sock_ok("regionald", &format!("SET_KEYBOARD\t{}", token(&a("v"))?))?; ok() }

        /* Users (nuubos-usersd). */
        "user.create" => { sock_ok("usersd", &format!("CREATE\t{}", plain(a("name").trim(), 32)?))?; ok() }
        "user.rename" => { sock_ok("usersd", &format!("SET_NAME\t{}\t{}", token(&a("id"))?, plain(a("name").trim(), 32)?))?; ok() }
        "user.avatar" => { sock_ok("usersd", &format!("SET_AVATAR\t{}\t{}", token(&a("id"))?, plain(&a("spec"), 200)?))?; ok() }
        "user.delete" => { sock_ok("usersd", &format!("DELETE\t{}\tCONFIRM", token(&a("id"))?))?; ok() }
        "user.default" => { sock_ok("usersd", &format!("SET_DEFAULT\t{}", token(&a("id"))?))?; ok() }
        "user.loginmode" => { sock_ok("usersd", &format!("SET_LOGIN_MODE\t{}", one_of(&a("v"), &["select", "default"])?))?; ok() }
        "user.activate" => { sock_ok("usersd", &format!("ACTIVATE\t{}", token(&a("id"))?))?; ok() }

        /* Moods (nuubos-themectl announces the choice to nuubUI). */
        "mood.select" => { tool_ok("nuubos-themectl", &["select", &user()?, &token(&a("id"))?])?; done("themes") }
        "mood.remove" => { tool_ok("nuubos-themectl", &["remove", &token(&a("id"))?])?; done("themes") }

        /* Remote services and the device credential. */
        "remote.set" => {
            let s = one_of(&a("svc"), &["ssh", "smb", "web"])?;
            let v = flag(&a("v"))?;
            if s == "web" && v == "0" {
                /* Turning the Web administration off ends this page. */
                return later("nuubos-remotectl", vec!["set".into(), "web".into(), "0".into()]);
            }
            tool_ok("nuubos-remotectl", &["set", &s, v])?;
            done("remote")
        }
        "remote.regenerate" => { tool_ok("nuubos-remotectl", &["credential", "regenerate"])?; done("remote") }
        /* The key line goes on stdin; nuubos-remotectl checks it. */
        "ssh.key.add" => {
            let key = plain(a("key").trim(), 8192)?;
            tool(&svc::bin("nuubos-remotectl"), &["keys", "add"], Some(&format!("{key}\n"))).map_err(|e| format!("sshkey-{e}"))?;
            done("ssh_keys")
        }
        "ssh.key.remove" => {
            tool_ok("nuubos-remotectl", &["keys", "remove", &token(&a("id"))?]).map_err(|e| format!("sshkey-{e}"))?;
            done("ssh_keys")
        }

        /* Syncthing (per user). */
        "sync.enable" => { sock_ok("syncd", if flag(&a("v"))? == "1" { "ENABLE" } else { "DISABLE" })?; done("sync") }
        "sync.folder" => {
            let kind = one_of(&a("kind"), &["saves", "states", "screenshots", "library", "history", "roms", "roms-sd2"])?;
            sock_ok("syncd", &format!("FOLDER\t{kind}\t{}", flag(&a("v"))?))?;
            done("sync")
        }
        "sync.internet" => { sock_ok("syncd", &format!("INTERNET\t{}", flag(&a("v"))?))?; done("sync") }
        "sync.relays" => { sock_ok("syncd", &format!("RELAYS\t{}", flag(&a("v"))?))?; done("sync") }
        /* Adding a device reconfigures Syncthing: a longer wait. */
        "sync.add" => {
            svc::reply_ok(&sock_timeout("syncd", &format!("ADD\t{}", token(&a("device"))?), Duration::from_secs(30))?)?;
            done("sync")
        }
        "sync.ignore" => {
            svc::reply_ok(&sock_timeout("syncd", &format!("IGNORE\t{}", token(&a("device"))?), Duration::from_secs(30))?)?;
            done("sync")
        }
        "sync.remove" => {
            svc::reply_ok(&sock_timeout("syncd", &format!("REMOVE\t{}", token(&a("device"))?), Duration::from_secs(30))?)?;
            done("sync")
        }
        /* An existing folder of the user's other devices for saves, states
         * screenshots or a ROM folder ("" = nuubOS's own); USE takes an
         * offered one. */
        "sync.folder_id" => {
            let kind = one_of(&a("kind"), &["saves", "states", "screenshots", "roms", "roms-sd2"])?;
            let id = plain(a("id").trim(), 199)?;
            let id = if id.is_empty() { "-".to_owned() } else { id };
            svc::reply_ok(&sock_timeout("syncd", &format!("FOLDER_ID\t{kind}\t{id}"), Duration::from_secs(30))?)?;
            done("sync")
        }
        "sync.use" => {
            let kind = one_of(&a("kind"), &["saves", "states", "screenshots", "roms", "roms-sd2"])?;
            let folder = plain(&a("folder"), 199)?;
            svc::reply_ok(&sock_timeout("syncd", &format!("USE\t{folder}\t{kind}"), Duration::from_secs(30))?)?;
            done("sync")
        }
        "sync.decline" => {
            let folder = plain(&a("folder"), 199)?;
            svc::reply_ok(&sock_timeout("syncd", &format!("DECLINE\t{folder}"), Duration::from_secs(30))?)?;
            done("sync")
        }

        /* Tailscale (nuubos-tsd): download, on/off, sign in/out. */
        "ts.install" | "ts.check" | "ts.cancel" | "ts.remove" | "ts.enable" | "ts.disable" | "ts.login" | "ts.logout" => {
            let cmd = op[3..].to_ascii_uppercase();
            svc::reply_ok(&sock_timeout("tsd", &cmd, Duration::from_secs(30))?)?;
            done("tailscale")
        }

        /* Wi-Fi (org.nuubOS.Wifi1). */
        "wifi.enable" => { call0("Wifi", "SetEnabled", &(flag(&a("v"))? == "1",))?; ok() }
        "wifi.scan" => { call0("Wifi", if flag(&a("v"))? == "1" { "StartScanSession" } else { "StopScanSession" }, &())?; ok() }
        "wifi.connect" => {
            let ssid = plain(&a("ssid"), 32)?;
            let pw = plain(&a("password"), 64)?;
            call0("Wifi", "Connect", &(ssid.as_str(), pw.as_str(), flag(&a("hidden")).unwrap_or("0") == "1"))?; ok()
        }
        "wifi.disconnect" => { call0("Wifi", "Disconnect", &())?; ok() }
        "wifi.forget" => { call0("Wifi", "Forget", &(plain(&a("ssid"), 32)?.as_str(),))?; ok() }
        "wifi.wps" => { call0("Wifi", "StartWps", &(plain(&a("ssid"), 32)?.as_str(),))?; ok() }
        "wifi.wpscancel" => { call0("Wifi", "CancelWps", &())?; ok() }
        "wifi.ip" => {
            let mode = one_of(&a("mode"), &["dhcp", "manual"])?;
            call0("Wifi", "SetIpConfiguration", &(plain(&a("ssid"), 32)?.as_str(), mode.as_str(), plain(&a("address"), 15)?.as_str(),
                int(&a("prefix"), 0, 32).unwrap_or(24) as u32, plain(&a("gateway"), 15)?.as_str(), plain(&a("dns"), 128)?.as_str()))?;
            ok()
        }

        /* Bluetooth (org.nuubOS.Bluetooth1). */
        "bt.enable" => { call0("Bluetooth", "SetEnabled", &(flag(&a("v"))? == "1",))?; ok() }
        "bt.discover" => { call0("Bluetooth", if flag(&a("v"))? == "1" { "StartDiscoverySession" } else { "StopDiscoverySession" }, &())?; ok() }
        "bt.pair" => { call0("Bluetooth", "PairAndConnect", &(token(&a("address"))?.as_str(),))?; ok() }
        "bt.pin" => { call0("Bluetooth", "PairWithPin", &(token(&a("address"))?.as_str(), plain(&a("pin"), 16)?.as_str()))?; ok() }
        "bt.connect" => { call0("Bluetooth", "Connect", &(token(&a("address"))?.as_str(),))?; ok() }
        "bt.disconnect" => { call0("Bluetooth", "Disconnect", &(token(&a("address"))?.as_str(),))?; ok() }
        "bt.forget" => { call0("Bluetooth", "Forget", &(token(&a("address"))?.as_str(),))?; ok() }

        /* Controllers, rumble, lighting. */
        "ctl.assign" => {
            call0("Controllers", "SetPlayerPreference", &(int(&a("player"), 1, 8)? as i32, plain(&a("id"), 128)?.as_str(), plain(&a("name"), 128)?.as_str()))?; ok()
        }
        "ctl.deadzone" => {
            call0("Controllers", "SetDeadzone", &(plain(&a("id"), 128)?.as_str(), one_of(&a("stick"), &["left", "right"])?.as_str(), int(&a("v"), 0, 50)? as i32))?;
            done("controllers")
        }
        "ctl.resetmapping" => { call0("Controllers", "ResetMapping", &(plain(&a("id"), 128)?.as_str(),))?; done("controllers") }
        "rumble.enable" => { call0("Rumble", "SetEnabled", &(flag(&a("v"))? == "1",))?; ok() }
        "rumble.mode" => { call0("Rumble", "SetMode", &(one_of(&a("v"), &["off", "light", "strong"])?,))?; ok() }
        "rumble.test" => { call0("Rumble", "Test", &())?; ok() }
        "light.effect" => { call0("Lighting", "SetEffect", &(token(&a("v"))?.as_str(),))?; ok() }
        "light.color" => { call0("Lighting", "SetColor", &(token(&a("v"))?.as_str(),))?; ok() }
        "light.brightness" => { call0("Lighting", "SetBrightness", &(int(&a("v"), 0, 100)? as i32,))?; ok() }
        "light.speed" => { call0("Lighting", "SetSpeed", &(one_of(&a("v"), &["slow", "normal", "fast"])?.as_str(),))?; ok() }
        "light.signal" => { call0("Lighting", "SetSignal", &(token(&a("category"))?.as_str(), flag(&a("v"))? == "1"))?; ok() }

        /* Emulation (nuubos-emud). */
        "emu.set" => { sock_ok("emud", &format!("SET\t{}\t{}", emu_key(&a("key"))?, plain(&a("value"), 256)?))?; done("emu_settings") }
        "emu.quit" => { sock_ok("emud", "QUIT")?; ok() }
        "game.set" => {
            sock_ok("emud", &format!("GAME_SET\t{}\t{}\t{}\t{}", game_id(&a("id"))?, token(&a("system"))?,
                settings_key(&a("key"))?, plain(&a("value"), 64)?))?;
            ok()
        }
        "game.reset" => { sock_ok("emud", &format!("GAME_RESET\t{}", game_id(&a("id"))?))?; ok() }
        "system.set" => {
            sock_ok("emud", &format!("SYSTEM_SET\t{}\t{}\t{}", token(&a("system"))?,
                settings_key(&a("key"))?, plain(&a("value"), 64)?))?;
            ok()
        }
        "system.reset" => { sock_ok("emud", &format!("SYSTEM_RESET\t{}", token(&a("system"))?))?; ok() }
        "system.reset_games" => { sock_ok("emud", &format!("SYSTEM_RESET_GAMES\t{}", token(&a("system"))?))?; ok() }

        /* RetroAchievements and metadata. Passwords go on stdin. */
        "ra.login" => {
            tool(&svc::bin("nuubos-achievementsctl"), &["login", &user()?, &plain(a("name").trim(), 64)?], Some(&plain(&a("password"), 256)?))?;
            done("ra")
        }
        "ra.logout" => { tool_ok("nuubos-achievementsctl", &["logout", &user()?])?; done("ra") }
        "ra.set" => { tool_ok("nuubos-achievementsctl", &["set", &user()?, &one_of(&a("key"), &["enabled", "hardcore", "offline"])?, flag(&a("v"))?])?; done("ra") }
        "scraper.provider" => { tool_ok("nuubos-scraper", &["provider", "set", &one_of(&a("v"), &["screenscraper", "thegamesdb", "libretro"])?])?; done("scraper") }
        "scraper.region" => { tool_ok("nuubos-scraper", &["region", "set", &one_of(&a("v"), &["auto", "us", "eu", "jp", "wor"])?])?; done("scraper") }
        "scraper.account" => {
            tool(&svc::bin("nuubos-scraper"), &["account", "set", &plain(a("name").trim(), 64)?], Some(&plain(&a("password"), 256)?))?;
            done("scraper")
        }
        "scraper.signout" => { tool_ok("nuubos-scraper", &["account", "clear"])?; done("scraper") }
        "scraper.apikey" => {
            let key = plain(a("key").trim(), 256)?;
            if key.is_empty() { tool_ok("nuubos-scraper", &["apikey", "clear"])?; } else { tool(&svc::bin("nuubos-scraper"), &["apikey", "set"], Some(&key))?; }
            done("scraper")
        }
        "scraper.bulk" => {
            let scope = a("scope");
            let good = ["all", "lang", "missing"].contains(&scope.as_str())
                || scope.strip_prefix("missing:").map(|n| int(n, 1, 100_000_000).is_ok()).unwrap_or(false)
                || scope.strip_prefix("system:").map(|s| token(s).is_ok()).unwrap_or(false);
            if !good { return Err("scope".into()); }
            job("scrape-bulk", &[&scope])
        }
        "scraper.game" => {
            let id = game_id(&a("id"))?;
            match a("mode").as_str() {
                "" => job("scrape-game", &[&id]),
                m => job("scrape-game", &[&id, &one_of(m, &["force", "fill"])?]),
            }
        }
        "scraper.search" => { tool(&svc::bin("nuubos-scraper"), &["search", &game_id(&a("id"))?], Some(&plain(a("name").trim(), 128)?))?; ok() }
        "scraper.match" => { tool_ok("nuubos-scraper", &[&one_of(&a("v"), &["accept", "reject", "clear"])?, &game_id(&a("id"))?])?; ok() }

        /* Library (nuubos-libraryd). */
        "lib.scan" => { sock_ok("libraryd", "SCAN")?; ok() }
        "lib.unavailable" => { sock_ok("libraryd", &format!("SHOW_UNAVAILABLE\t{}", flag(&a("v"))?))?; ok() }
        "lib.wrap" => { sock_ok("libraryd", &format!("WRAP_ROWS\t{}", flag(&a("v"))?))?; ok() }
        "lib.delete" => { sock_ok("libraryd", &format!("DELETE\t{}\tCONFIRM", game_id(&a("id"))?))?; ok() }
        "lib.statsreset" => { sock_ok("libraryd", &format!("STATS_RESET\t{}", game_id(&a("id"))?))?; ok() }
        "coll.create" => { sock_ok("libraryd", &format!("COLLECTION_CREATE\t{}", plain(a("name").trim(), 64)?))?; ok() }
        "coll.rename" => { sock_ok("libraryd", &format!("COLLECTION_RENAME\t{}\t{}", token(&a("id"))?, plain(a("name").trim(), 64)?))?; ok() }
        "coll.delete" => { sock_ok("libraryd", &format!("COLLECTION_DELETE\t{}", token(&a("id"))?))?; ok() }
        "coll.add" => { sock_ok("libraryd", &format!("COLLECTION_ADD\t{}\t{}", token(&a("id"))?, game_id(&a("game"))?))?; ok() }
        "coll.remove" => { sock_ok("libraryd", &format!("COLLECTION_REMOVE\t{}\t{}", token(&a("id"))?, game_id(&a("game"))?))?; ok() }

        /* Background jobs. */
        "job.cancel" => { sock_ok("jobd", &format!("CANCEL\t{}", int(&a("id"), 1, i64::MAX)?))?; ok() }
        "job.retry" => { sock_ok("jobd", &format!("RETRY\t{}", int(&a("id"), 1, i64::MAX)?))?; ok() }
        "diag.bundle" => job("support-bundle", &[]),

        /* Software update and recovery. */
        "update.check" => { tool_ok("nuubos-updatectl", &["check"])?; done("update") }
        "update.download" => job("update-download", &[]),
        "update.channel" => { tool_ok("nuubos-updatectl", &["channel", &one_of(&a("v"), &["stable", "rc", "nightly"])?])?; done("update") }
        "update.auto" => { tool_ok("nuubos-updatectl", &["auto", if one_of(&a("v"), &["0", "1"])? == "1" { "on" } else { "off" }])?; done("update") }
        "update.install" => job("update-install", &[]),
        "rec.repair" => { tool_ok("nuubos-recoveryctl", &["repair"])?; ok() }
        "rec.resetuser" => later("nuubos-recoveryctl", vec!["reset-user".into(), user()?]),
        "rec.factory" => later("nuubos-recoveryctl", vec!["factory-reset".into(), one_of(&a("v"), &["settings", "userdata"])?]),

        /* Files (nuubos-filesctl; copy and move are jobs). */
        "files.mkdir" => {
            let dir = files::user_path(&a("dir")).ok_or("path")?;
            let name = a("name");
            if !files::valid_name(&name) { return Err("name".into()); }
            tool_ok("nuubos-filesctl", &["mkdir", &format!("{dir}/{name}")])?; ok()
        }
        "files.rename" => {
            let name = a("name");
            if !files::valid_name(&name) { return Err("name".into()); }
            tool_ok("nuubos-filesctl", &["rename", &files::user_path(&a("path")).ok_or("path")?, &name])?; ok()
        }
        "files.delete" => { tool_ok("nuubos-filesctl", &["delete", &files::user_path(&a("path")).ok_or("path")?])?; ok() }
        "files.copy" | "files.move" => {
            let src = files::user_path(&a("path")).ok_or("path")?;
            let dst = files::user_path(&a("dir")).ok_or("path")?;
            job(if op == "files.copy" { "file-copy" } else { "file-move" }, &[&src, &dst])
        }
        "media.eject" => { tool_ok("nuubos-mediactl", &["eject", &plain(&a("name"), 64)?])?; ok() }
        "share.add" => {
            let name = token(&a("name"))?;
            let addr = plain(&a("address"), 256)?;
            if !addr.starts_with("//") { return Err("address".into()); }
            let u = plain(&a("user"), 64)?;
            let mut args = vec!["add", name.as_str(), addr.as_str()];
            if !u.is_empty() { args.push(u.as_str()); }
            tool(&svc::bin("nuubos-sharesctl"), &args, Some(&plain(&a("password"), 128)?))?;
            done("shares")
        }
        "share.mount" | "share.umount" | "share.remove" => {
            tool_ok("nuubos-sharesctl", &[op.trim_start_matches("share."), &token(&a("name"))?])?;
            done("shares")
        }

        _ => Err("op".into()),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn checks() {
        assert!(token("Europe/Rome").is_ok());
        assert!(token("../etc").is_err());
        assert!(token("a\tb").is_err());
        assert!(plain("Mario\nWorld", 64).is_err());
        assert!(game_id("0123456789abcdef").is_ok());
        assert!(game_id("0123456789abcdeg").is_err());
        assert_eq!(int("42", 0, 50).unwrap(), 42);
        assert!(int("51", 0, 50).is_err());
        assert!(emu_key("hotkey_quit").is_ok());
        assert!(emu_key("core_path").is_err());
        assert_eq!(catalog_lang("it"), "it");
        assert_eq!(catalog_lang("pt-br"), "pt-br");
        assert_eq!(catalog_lang("es-419"), "es-419");
        assert_eq!(catalog_lang("zh-cn"), "zh-cn");
        assert_eq!(catalog_lang("../x"), "en");
        assert_eq!(catalog_lang("zh-cn/../../x"), "en");
        assert_eq!(catalog_lang("ITA"), "en");
    }

    #[test]
    fn unknown_op() {
        assert_eq!(action(&json!({ "op": "rm -rf" })).unwrap_err(), "op");
        assert_eq!(action(&json!({ "op": "system.sleep", "v": "1000" })).unwrap_err(), "value");
        assert_eq!(action(&json!({ "op": "system.hypersleep", "v": "2" })).unwrap_err(), "value");
    }
}
