/*
 * Screen casting preferences in nuubUI (EPIC-029): Settings → Display &
 * Audio, section SCREEN CASTING (Sound on the TV, Casting Latency, Miracast
 * Encoder). Casting itself starts from the Quick Menu (Cast Screen), which
 * is reachable everywhere, games included.
 *
 * nuubos-castd owns the capability: this module subscribes to its snapshot
 * (/run/nuubos/castd.sock, SUBSCRIBE) and sends its commands. No polling:
 * the encoder download progress arrives as new snapshots.
 */

use crate::{tr, HomeWindow};
use slint::ComponentHandle;
use std::io::{BufRead, BufReader, Write};
use std::os::unix::net::UnixStream;
use std::thread;
use std::time::Duration;

const SOCKET: &str = "/run/nuubos/castd.sock";
const LATENCIES: [&str; 3] = ["low", "normal", "smooth"];

#[derive(Clone, Default)]
struct Snapshot {
    user: bool,
    audio: bool,
    latency: String,
    h264: String,
    job: String,
    progress: i32,
}

fn parse(text: &str) -> Snapshot {
    let mut s = Snapshot::default();
    for line in text.lines() {
        let Some((k, v)) = line.split_once('=') else { continue };
        match k {
            "user" => s.user = v == "1",
            "audio" => s.audio = v == "1",
            "latency" => s.latency = v.to_owned(),
            "h264" => s.h264 = v.to_owned(),
            "h264_job" => s.job = v.to_owned(),
            "h264_progress" => s.progress = v.parse().unwrap_or(0),
            _ => {}
        }
    }
    s
}

fn apply(ui: &HomeWindow, s: Option<Snapshot>) {
    let Some(s) = s else {
        if ui.get_cast_available() {
            /* The focus leaves the rows that disappear. */
            ui.set_connectivity_selected_index(ui.get_connectivity_selected_index().min(5));
        }
        ui.set_cast_available(false);
        return;
    };
    let state = if !s.job.is_empty() { s.job.clone() } else { s.h264.clone() };
    ui.set_cast_available(s.user);
    ui.set_cast_audio(s.audio);
    ui.set_cast_latency(s.latency.as_str().into());
    ui.set_cast_h264(state.as_str().into());
    let value = match state.as_str() {
        "installed" => tr(ui, 1220, "Installed"),
        "installing" => tr(ui, 1221, "Downloading {0}%").replace("{0}", &s.progress.to_string()),
        "removing" => "…".to_owned(),
        _ => tr(ui, 653, "Download"),
    };
    ui.set_cast_encoder_value(value.into());
}

pub fn start_listener(ui: &HomeWindow) {
    let weak = ui.as_weak();
    thread::spawn(move || {
        let mut pause = 500;
        loop {
            if let Ok(mut s) = UnixStream::connect(SOCKET) {
                if s.write_all(b"SUBSCRIBE\n").is_ok() {
                    pause = 500;
                    let mut buf = String::new();
                    for line in BufReader::new(s).lines() {
                        let Ok(l) = line else { break };
                        if l == "end=1" {
                            let snap = parse(&buf);
                            buf.clear();
                            let w = weak.clone();
                            let _ = slint::invoke_from_event_loop(move || {
                                if let Some(ui) = w.upgrade() {
                                    apply(&ui, Some(snap));
                                }
                            });
                        } else {
                            buf.push_str(&l);
                            buf.push('\n');
                        }
                    }
                }
            }
            /* castd absent or restarting: the section hides. */
            let w = weak.clone();
            let _ = slint::invoke_from_event_loop(move || {
                if let Some(ui) = w.upgrade() {
                    apply(&ui, None);
                }
            });
            thread::sleep(Duration::from_millis(pause));
            pause = (pause * 2).min(30_000);
        }
    });
}

/* Fire and forget: the new snapshot shows the result. */
fn command(line: String) {
    thread::spawn(move || {
        let Ok(mut s) = UnixStream::connect(SOCKET) else { return };
        s.set_read_timeout(Some(Duration::from_secs(3))).ok();
        if s.write_all(line.as_bytes()).and_then(|_| s.write_all(b"\n")).is_err() {
            return;
        }
        let mut reply = String::new();
        let _ = BufReader::new(s).read_line(&mut reply);
        if !reply.starts_with("OK") {
            eprintln!("nuubui-home: cast {} -> {}", line.replace('\t', " "), reply.trim());
        }
    });
}

pub fn toggle_audio(ui: &HomeWindow) {
    let on = !ui.get_cast_audio();
    ui.set_cast_audio(on);
    command(format!("SET\taudio\t{}", if on { 1 } else { 0 }));
}

pub fn step_latency(ui: &HomeWindow, step: i32) {
    let current = LATENCIES.iter().position(|l| *l == ui.get_cast_latency().as_str()).unwrap_or(1) as i32;
    let next = (current + step).rem_euclid(LATENCIES.len() as i32) as usize;
    ui.set_cast_latency(LATENCIES[next].into());
    command(format!("SET\tlatency\t{}", LATENCIES[next]));
}

/* Download (Cisco's binary) / cancel the download / remove it. */
pub fn encoder_action(ui: &HomeWindow) {
    match ui.get_cast_h264().as_str() {
        "installing" => command("H264_CANCEL".into()),
        "installed" => command("H264_REMOVE".into()),
        "removing" => {}
        _ => command("H264_INSTALL".into()),
    }
}
