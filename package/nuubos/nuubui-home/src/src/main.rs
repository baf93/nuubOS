mod library;
mod splash;
slint::include_modules!();

use slint::{ComponentHandle, Image, Model, ModelRc, SharedString, Timer, VecModel};
use std::fs::{self, File};
use std::rc::Rc;
use std::io::{BufRead, BufReader, Read, Write};
use std::net::Ipv4Addr;
use std::path::Path;
use std::os::unix::net::UnixStream;
use std::process::{Command, Stdio};
use std::sync::{
    atomic::{AtomicBool, Ordering},
    Arc, Condvar, Mutex, OnceLock,
};
use std::thread;
use std::time::Duration;

const UI_READY: &str = "/run/nuubos/ui-ready";
const UI_CONTROL: &str = "/run/nuubos/ui-control";
const STATUS_STATE: &str = "/run/nuubos/statusd.state";
const STATUS_SOCKET: &str = "/run/nuubos/statusd.sock";
const SYSTEM_SOCKET: &str = "/run/nuubos/systemd.sock";
const INPUT_SOCKET: &str = "/run/nuubos/inputd.sock";
const DISPLAYCTL: &str = "/usr/bin/nuubos-displayctl";
const AUDIO_TEST_TONE: &str = "/usr/bin/nuubos-audio-test-tone";
const AUDIO_SOCKET: &str = "/run/nuubos/audiod.sock";
const LOCALIZATION_SOCKET: &str = "/run/nuubos/localizationd.sock";
const USERS_SOCKET: &str = "/run/nuubos/usersd.sock";
const REGIONAL_SOCKET: &str = "/run/nuubos/regionald.sock";
const UI_CONTEXT: &str = "/run/nuubos/ui-context";
const I18N_DIR: &str = "/usr/share/nuubos/i18n";

const LINE_STAGE_MS: u64 = 220;
const CURTAIN_STAGE_MS: u64 = 460;
const STAGE_GAP_MS: u64 = 18;

static SYSTEM_INFO_GATE: OnceLock<Arc<(Mutex<bool>, Condvar)>> = OnceLock::new();
/* Initial setup (OOB) is running: controller navigation belongs to it. The
 * state itself is owned by nuubos-usersd (setup_complete). */
static OOB_ACTIVE: AtomicBool = AtomicBool::new(false);

fn set_system_info_live(active: bool) {
    if let Some(gate) = SYSTEM_INFO_GATE.get() {
        let (lock, cv) = &**gate;
        let mut state = lock.lock().unwrap();
        let changed = *state != active;
        *state = active;
        if changed { cv.notify_one(); }
    }
}

fn start_system_info_live(ui: &HomeWindow, gate: Arc<(Mutex<bool>, Condvar)>) {
    let weak = ui.as_weak();
    thread::spawn(move || loop {
        let (lock, cv) = &*gate;
        let mut active = lock.lock().unwrap();
        while !*active {
            active = cv.wait(active).unwrap();
        }
        drop(active);

        if let Ok(reply) = system_command("STATUS") {
            let snapshot = parse_system_snapshot(&reply);
            let weak = weak.clone();
            let _ = slint::invoke_from_event_loop(move || {
                if let Some(ui) = weak.upgrade() {
                    apply_system_snapshot(&ui, &snapshot);
                }
            });
        }

        let active = lock.lock().unwrap();
        let _ = cv.wait_timeout(active, Duration::from_secs(1)).unwrap();
    });
}

#[derive(Clone)]
struct TopbarState {
    time: String,
    user_name: String,
    wifi_state: String,
    battery_percent: i32,
    battery_state: String,
}

impl Default for TopbarState {
    fn default() -> Self {
        Self {
            time: "--:--".to_owned(),
            user_name: "Player".to_owned(),
            wifi_state: "disconnected".to_owned(),
            battery_percent: -1,
            battery_state: "unknown".to_owned(),
        }
    }
}

#[derive(Clone)]
struct WifiProductState {
    enabled: bool,
    state: String,
    ssid: String,
    ipv4: String,
    signal_dbm: i32,
}

impl Default for WifiProductState {
    fn default() -> Self {
        Self {
            enabled: false,
            state: "unavailable".to_owned(),
            ssid: String::new(),
            ipv4: String::new(),
            signal_dbm: 0,
        }
    }
}

#[derive(Clone)]
struct BluetoothProductState {
    present: bool,
    powered: bool,
    address: String,
    connected_count: u32,
    paired_count: u32,
}

impl Default for BluetoothProductState {
    fn default() -> Self {
        Self {
            present: false,
            powered: false,
            address: String::new(),
            connected_count: 0,
            paired_count: 0,
        }
    }
}


#[derive(Debug, Clone)]
struct SystemSnapshot {
    version: String,
    build: String,
    profile: String,
    effective_profile: String,
    auto_battery_threshold: i32,
    screensaver_after_min: i32,
    sleep_after_min: i32,
    poweroff_after_min: i32,
    rtc_wakeup: bool,
    cpu_freq_khz: i64,
    gpu_freq_hz: i64,
    cpu_temp_millic: i64,
    gpu_temp_millic: i64,
    mem_total_kib: i64,
    mem_available_kib: i64,
    uptime_sec: i64,
    device_model: String,
    kernel: String,
    network: String,
    storage_mode: String,
    storage_active: String,
    storage_health: String,
    tf2_state: String,
    tf2_cid: String,
    storage_action_required: String,
    backup_policy: String,
    backup_due: String,
    backup_reason: String,
    storage_job_state: String,
    storage_job_action: String,
    storage_job_exit: i32,
    userdata_total: u64,
    userdata_free: u64,
    idle_stage: String,
}

impl Default for SystemSnapshot {
    fn default() -> Self {
        Self {
            version: "Unavailable".into(),
            build: "Unavailable".into(),
            profile: "auto".into(),
            effective_profile: "auto".into(),
            auto_battery_threshold: 0,
            screensaver_after_min: 0,
            sleep_after_min: 0,
            poweroff_after_min: 0,
            rtc_wakeup: false,
            cpu_freq_khz: -1,
            gpu_freq_hz: -1,
            cpu_temp_millic: -1,
            gpu_temp_millic: -1,
            mem_total_kib: -1,
            mem_available_kib: -1,
            uptime_sec: -1,
            device_model: "Unavailable".into(),
            kernel: "Unavailable".into(),
            network: "Unavailable".into(),
            storage_mode: "Unknown".into(),
            storage_active: "Unknown".into(),
            storage_health: "Unknown".into(),
            tf2_state: "Unknown".into(),
            tf2_cid: String::new(),
            storage_action_required: "NONE".into(),
            backup_policy: "Unknown".into(),
            backup_due: "Unknown".into(),
            backup_reason: "Unknown".into(),
            storage_job_state: "idle".into(),
            storage_job_action: "none".into(),
            storage_job_exit: 0,
            userdata_total: 0,
            userdata_free: 0,
            idle_stage: "active".into(),
        }
    }
}

fn system_command(command: &str) -> Result<String, Box<dyn std::error::Error>> {
    let mut stream = UnixStream::connect(SYSTEM_SOCKET)?;
    stream.set_read_timeout(Some(Duration::from_millis(500)))?;
    stream.set_write_timeout(Some(Duration::from_millis(500)))?;
    stream.write_all(command.as_bytes())?;
    if !command.ends_with('\n') {
        stream.write_all(b"\n")?;
    }
    stream.shutdown(std::net::Shutdown::Write)?;
    let mut reply = String::new();
    stream.read_to_string(&mut reply)?;
    Ok(reply)
}

fn localization_command(command: &str) -> std::io::Result<String> {
    let mut stream = UnixStream::connect(LOCALIZATION_SOCKET)?;
    stream.set_read_timeout(Some(Duration::from_millis(500)))?;
    stream.set_write_timeout(Some(Duration::from_millis(500)))?;
    stream.write_all(command.as_bytes())?;
    if !command.ends_with('\n') {
        stream.write_all(b"\n")?;
    }
    stream.shutdown(std::net::Shutdown::Write)?;
    let mut reply = String::new();
    stream.read_to_string(&mut reply)?;
    Ok(reply)
}

fn language_name(code: &str) -> &'static str {
    match code {
        "it" => "Italiano",
        "fr" => "Français",
        "de" => "Deutsch",
        "es" => "Español",
        "pt" => "Português",
        "nl" => "Nederlands",
        _ => "English",
    }
}

fn load_i18n_catalog(code: &str) -> Vec<SharedString> {
    fn load(path: &str) -> Vec<Option<String>> {
        let Ok(file) = File::open(path) else { return Vec::new(); };
        let mut values: Vec<Option<String>> = Vec::new();
        for line in BufReader::new(file).lines() {
            let Ok(line) = line else { continue; };
            if line.trim().is_empty() || line.starts_with('#') { continue; }
            let Some((key, value)) = line.split_once('=') else { continue; };
            let Ok(index) = key.trim().parse::<usize>() else { continue; };
            if values.len() <= index { values.resize(index + 1, None); }
            if !value.is_empty() { values[index] = Some(value.to_owned()); }
        }
        values
    }

    let english = load(&format!("{}/en.lang", I18N_DIR));
    let selected = if code == "en" {
        Vec::new()
    } else {
        load(&format!("{}/{}.lang", I18N_DIR, code))
    };
    let len = english.len().max(selected.len());
    (0..len)
        .map(|index| {
            selected.get(index).and_then(|value| value.clone())
                .or_else(|| english.get(index).and_then(|value| value.clone()))
                .unwrap_or_default()
                .into()
        })
        .collect()
}

fn apply_language(ui: &HomeWindow, code: &str) {
    let values = load_i18n_catalog(code);
    if !values.is_empty() {
        ui.set_i18n_strings(ModelRc::from(Rc::new(VecModel::from(values))));
    }
    ui.set_ui_language_code(code.into());
    ui.set_ui_language_name(language_name(code).into());
    library::relocalize(ui);
}

fn tr(ui: &HomeWindow, index: usize, fallback: &str) -> String {
    ui.get_i18n_strings()
        .row_data(index)
        .map(|value| value.to_string())
        .filter(|value| !value.is_empty())
        .unwrap_or_else(|| fallback.to_owned())
}

fn tr_arg(ui: &HomeWindow, index: usize, fallback: &str, arg: &str) -> String {
    tr(ui, index, fallback).replace("{0}", arg)
}

fn parse_localization_snapshot(reply: &str) -> String {
    for line in reply.lines() {
        if let Some(value) = line.strip_prefix("language=") {
            return value.trim().to_owned();
        }
    }
    "en".to_owned()
}

fn start_localization_listener(ui: &HomeWindow) {
    let weak = ui.as_weak();
    thread::spawn(move || loop {
        match UnixStream::connect(LOCALIZATION_SOCKET) {
            Ok(mut stream) => {
                if stream.write_all(b"SUBSCRIBE\n").is_err() {
                    thread::sleep(Duration::from_millis(500));
                    continue;
                }
                let reader = BufReader::new(stream);
                let mut snapshot = String::new();
                for line in reader.lines() {
                    let Ok(line) = line else { break; };
                    if line.starts_with("language=") {
                        snapshot = line;
                    } else if line.starts_with("available=") {
                        let code = parse_localization_snapshot(&snapshot);
                        let weak = weak.clone();
                        let _ = slint::invoke_from_event_loop(move || {
                            if let Some(ui) = weak.upgrade() {
                                apply_language(&ui, &code);
                            }
                        });
                        snapshot.clear();
                    }
                }
            }
            Err(_) => thread::sleep(Duration::from_millis(500)),
        }
    });
}

fn open_language_dropdown(ui: &HomeWindow) {
    open_settings_choice(
        ui,
        "ui-language",
        &tr(ui, 193, "Language"),
        vec![
            ("en".to_owned(), "English".to_owned()),
            ("it".to_owned(), "Italiano".to_owned()),
            ("fr".to_owned(), "Français".to_owned()),
            ("de".to_owned(), "Deutsch".to_owned()),
            ("es".to_owned(), "Español".to_owned()),
            ("pt".to_owned(), "Português".to_owned()),
            ("nl".to_owned(), "Nederlands".to_owned()),
        ],
        ui.get_ui_language_code().as_str(),
    );
}

#[derive(Clone,Default)] struct UEntry{id:String,name:String,spec:String,path:String,active:bool,default_user:bool}
#[derive(Clone,Default)] struct USnap{active:String,mode:String,default_user:String,last_user:String,setup:bool,selection:bool,switch_requested:bool,users:Vec<UEntry>}
#[derive(Clone,Default)] struct RSnap{timezone:String,auto_time:bool,keyboard:String,last_sync:i64,local_date:String,local_time:String}
fn product_command_timeout(path:&str,cmd:&str,timeout:Duration)->std::io::Result<String>{let mut x=UnixStream::connect(path)?;x.set_read_timeout(Some(timeout))?;x.write_all(cmd.as_bytes())?;if !cmd.ends_with('\n'){x.write_all(b"\n")?;}x.shutdown(std::net::Shutdown::Write)?;let mut r=String::new();x.read_to_string(&mut r)?;Ok(r)}
fn product_command(path:&str,cmd:&str)->std::io::Result<String>{product_command_timeout(path,cmd,Duration::from_secs(2))}
fn users_command(c:&str)->std::io::Result<String>{product_command(USERS_SOCKET,c)}
fn regional_command(c:&str)->std::io::Result<String>{product_command(REGIONAL_SOCKET,c)}
fn regional_sync_command()->std::io::Result<String>{product_command_timeout(REGIONAL_SOCKET,"SYNC_NOW",Duration::from_secs(15))}
/* Profile pictures are shown in circles. The software renderer clips to
 * rectangles only (border-radius is ignored), so every avatar is turned into a
 * round image here: centre square crop (cover), box downscale to at most
 * AVATAR_MAX_PX (largest circle 112 logical px x HDMI scale 3) and an
 * anti-aliased circular alpha mask. Slint then scales it to the circle size. */
const AVATAR_MAX_PX:u32=384;
fn round_avatar(src:&Image)->Option<Image>{
    let b=src.to_rgba8_premultiplied()?;
    let (w,h)=(b.width(),b.height());
    if w==0||h==0{return None;}
    let side=w.min(h);let (ox,oy)=((w-side)/2,(h-side)/2);
    let n=side.min(AVATAR_MAX_PX);
    let px=b.as_slice();
    let mut out=slint::SharedPixelBuffer::<slint::Rgba8Pixel>::new(n,n);
    let r=n as f32/2.0;
    for (i,o) in out.make_mut_slice().iter_mut().enumerate(){
        let (x,y)=(i as u32%n,i as u32/n);
        let (x0,x1)=(ox+x*side/n,ox+((x+1)*side/n).max(x*side/n+1));
        let (y0,y1)=(oy+y*side/n,oy+((y+1)*side/n).max(y*side/n+1));
        let mut acc=[0u32;4];
        for sy in y0..y1{for sx in x0..x1{let p=px[(sy*w+sx) as usize];acc[0]+=p.r as u32;acc[1]+=p.g as u32;acc[2]+=p.b as u32;acc[3]+=p.a as u32;}}
        let cnt=((x1-x0)*(y1-y0)) as f32;
        let (dx,dy)=(x as f32+0.5-r,y as f32+0.5-r);
        let cov=(r-(dx*dx+dy*dy).sqrt()+0.5).clamp(0.0,1.0)/cnt;
        let c=|v:u32|(v as f32*cov+0.5) as u8;
        *o=slint::Rgba8Pixel{r:c(acc[0]),g:c(acc[1]),b:c(acc[2]),a:c(acc[3])};
    }
    Some(Image::from_rgba8_premultiplied(out))
}
fn avatar_image(p:&str)->Image{if p.is_empty(){return Image::default();}let img=Image::load_from_path(Path::new(p)).unwrap_or_default();round_avatar(&img).unwrap_or(img)}
fn parse_users(r:&str)->USnap{let mut o=USnap{mode:"select".into(),setup:true,..Default::default()};for l in r.lines(){if let Some(v)=l.strip_prefix("active="){o.active=v.into();}else if let Some(v)=l.strip_prefix("login_mode="){o.mode=v.into();}else if let Some(v)=l.strip_prefix("default_user="){o.default_user=v.into();}else if let Some(v)=l.strip_prefix("last_user="){o.last_user=v.into();}else if let Some(v)=l.strip_prefix("setup_complete="){o.setup=v!="0";}else if let Some(v)=l.strip_prefix("selection_required="){o.selection=v=="1";}else if let Some(v)=l.strip_prefix("switch_requested="){o.switch_requested=v=="1";}else if let Some(v)=l.strip_prefix("user="){let f:Vec<&str>=v.split('\t').collect();if f.len()>=6{o.users.push(UEntry{id:f[0].into(),name:f[1].into(),spec:f[2].into(),path:f[3].into(),active:f[4]=="1",default_user:f[5]=="1"});}}}o}
fn apply_users(ui:&HomeWindow,s:&USnap,picker:&Arc<AtomicBool>){
    let rows:Vec<UserProfileEntry>=s.users.iter().map(|u|UserProfileEntry{
        id:u.id.clone().into(),
        name:u.name.clone().into(),
        avatar_spec:u.spec.clone().into(),
        avatar_path:u.path.clone().into(),
        avatar:avatar_image(&u.path),
        active:u.active,
        default_user:u.default_user
    }).collect();
    ui.set_users(ModelRc::from(Rc::new(VecModel::from(rows))));
    ui.set_user_count(s.users.len() as i32);
    ui.set_active_user_id(s.active.clone().into());
    ui.set_default_user_id(s.default_user.clone().into());
    ui.set_user_login_mode(s.mode.clone().into());
    ui.set_user_selection_required(s.selection);
    ui.set_user_switch_requested(s.switch_requested);
    ui.set_default_user_name(
        s.users.iter().find(|u|u.id==s.default_user)
            .map(|u|u.name.clone()).unwrap_or_default().into()
    );

    if let Some(a)=s.users.iter().find(|u|u.id==s.active){
        ui.set_current_user_name(a.name.clone().into());
        ui.set_current_user_avatar(avatar_image(&a.path));
        ui.set_current_user_avatar_available(!a.path.is_empty());
        if ui.get_profile_edit_user_id().as_str().is_empty()
            || ui.get_profile_edit_user_id().as_str()==a.id
        {
            ui.set_profile_edit_user_id(a.id.clone().into());
            ui.set_profile_edit_name(a.name.clone().into());
            ui.set_profile_edit_avatar_spec(a.spec.clone().into());
            ui.set_profile_edit_avatar(avatar_image(&a.path));
        }
    } else {
        ui.set_current_user_avatar(Image::default());
        ui.set_current_user_avatar_available(false);
    }
    /* Keep the user detail page live for a user other than the active one. */
    let edit=ui.get_profile_edit_user_id().to_string();
    if !edit.is_empty() && edit!=s.active {
        if let Some(u)=s.users.iter().find(|u|u.id==edit){
            ui.set_profile_edit_name(u.name.clone().into());
            ui.set_profile_edit_avatar_spec(u.spec.clone().into());
            ui.set_profile_edit_avatar(avatar_image(&u.path));
        }
    }

    /* Initial setup gates the session: no user picker while the OOB runs. */
    let oob=!s.setup;
    if oob!=ui.get_oob_active(){
        if oob{enter_oob(ui);}else{leave_oob(ui);}
    }
    if oob&&ui.get_settings_view()==32{
        ui.set_oob_index(ui.get_oob_index().clamp(0,oob_users_rows(ui)-1));
        update_oob_users_scroll(ui);
    }

    let open=!oob&&(s.selection||s.switch_requested);
    if open&&!ui.get_user_picker_open(){
        let i=s.users.iter()
            .position(|u|u.id==s.last_user)
            .or_else(||s.users.iter().position(|u|u.active))
            .or_else(||s.users.iter().position(|u|u.default_user))
            .unwrap_or(0) as i32;
        ui.set_user_picker_index(i);
        ui.set_user_picker_scroll(0);
    }
    picker.store(open,Ordering::SeqCst);
    ui.set_user_picker_forced(s.selection);
    ui.set_user_picker_open(open);
}
fn refresh_users(ui:&HomeWindow,p:&Arc<AtomicBool>){if let Ok(r)=users_command("STATUS"){apply_users(ui,&parse_users(&r),p);}}
fn start_users_listener(ui:&HomeWindow,picker:Arc<AtomicBool>){let weak=ui.as_weak();thread::spawn(move||loop{match UnixStream::connect(USERS_SOCKET){Ok(mut x)=>{if x.write_all(b"SUBSCRIBE\n").is_err(){thread::sleep(Duration::from_millis(500));continue;}let mut b=String::new();for l in BufReader::new(x).lines(){let Ok(l)=l else{break};b.push_str(&l);b.push('\n');if l=="end=1"{let snap=parse_users(&b);b.clear();let w=weak.clone();let p=picker.clone();let _=slint::invoke_from_event_loop(move||if let Some(ui)=w.upgrade(){apply_users(&ui,&snap,&p);});}}}Err(_)=>thread::sleep(Duration::from_millis(500))}});}
fn parse_regional(r:&str)->RSnap{let mut o=RSnap{timezone:"UTC".into(),auto_time:true,keyboard:"us".into(),last_sync:0,local_date:"---- -- --".into(),local_time:"--:--".into()};for l in r.lines(){let Some((k,v))=l.split_once('=') else{continue};match k{"timezone"=>o.timezone=v.into(),"automatic_time"=>o.auto_time=v=="1","keyboard_layout"=>o.keyboard=v.into(),"last_sync"=>o.last_sync=v.parse().unwrap_or(0),"local_date"=>o.local_date=v.into(),"local_time"=>o.local_time=v.into(),_=>{}}}o}
fn keyboard_label(c:&str)->&'static str{match c{"uk"=>"English (UK)","it"=>"Italiano","fr"=>"Français","de"=>"Deutsch","es"=>"Español","pt-latin1"=>"Português",_=>"English (US)"}}fn virtual_layout(c:&str)->&'static str{match c{"fr"=>"azerty","de"=>"qwertz",_=>"qwerty"}}
fn apply_regional(ui:&HomeWindow,s:&RSnap){ui.set_timezone(s.timezone.clone().into());ui.set_automatic_time(s.auto_time);ui.set_manual_date_label(s.local_date.clone().into());ui.set_manual_time_label(s.local_time.clone().into());ui.set_keyboard_layout_code(s.keyboard.clone().into());ui.set_keyboard_layout_label(keyboard_label(&s.keyboard).into());ui.set_keyboard_layout(virtual_layout(&s.keyboard).into());ui.set_last_time_sync_label(if s.last_sync>0{tr(ui,299,"Time synchronized").into()}else{tr(ui,302,"Never").into()});}
fn refresh_regional(ui:&HomeWindow){if let Ok(r)=regional_command("STATUS"){apply_regional(ui,&parse_regional(&r));}}
fn start_regional_listener(ui:&HomeWindow){let weak=ui.as_weak();thread::spawn(move||loop{match UnixStream::connect(REGIONAL_SOCKET){Ok(mut x)=>{if x.write_all(b"SUBSCRIBE\n").is_err(){thread::sleep(Duration::from_millis(500));continue;}let mut b=String::new();for l in BufReader::new(x).lines(){let Ok(l)=l else{break};b.push_str(&l);b.push('\n');if l.starts_with("clock_valid="){let q=parse_regional(&b);b.clear();let w=weak.clone();let _=slint::invoke_from_event_loop(move||if let Some(ui)=w.upgrade(){apply_regional(&ui,&q);});}}}Err(_)=>thread::sleep(Duration::from_millis(500))}});}
fn update_general_scroll(ui:&HomeWindow){
    /* General has 12 rows split in USER/DEVICE sections; the sectioned
     * capacity reserves the inline section header. */
    const COUNT:i32=12;
    let index=ui.get_general_index().clamp(0,COUNT-1);
    let rows=ui.get_settings_sectioned_visible_rows().max(1);
    ui.set_general_scroll_offset(guarded_scroll_offset(
        index,COUNT,rows,ui.get_general_scroll_offset().clamp(0,COUNT-1)
    ));
}
fn update_user_scroll(ui:&HomeWindow){ui.set_user_list_scroll(guarded_scroll_offset(ui.get_user_list_index(),ui.get_user_count()+1,ui.get_settings_list_visible_rows(),ui.get_user_list_scroll()));}fn update_avatar_scroll(ui:&HomeWindow){ui.set_avatar_picker_scroll(guarded_scroll_offset(ui.get_avatar_picker_index(),ui.get_avatar_choices().row_count() as i32,5,ui.get_avatar_picker_scroll()));}
fn users_page_view(ui:&HomeWindow)->i32{if ui.get_oob_active(){32}else{25}}
fn open_user(ui:&HomeWindow,u:UserProfileEntry,v:i32){ui.set_profile_edit_user_id(u.id);ui.set_profile_edit_name(u.name);ui.set_profile_edit_avatar_spec(u.avatar_spec);ui.set_profile_edit_avatar(u.avatar);ui.set_profile_index(0);ui.set_user_delete_confirm(false);navigate_settings_view(ui,v);}fn open_active_user(ui:&HomeWindow){for i in 0..ui.get_users().row_count(){if let Some(u)=ui.get_users().row_data(i){if u.active{open_user(ui,u,24);return;}}}}fn open_selected_user(ui:&HomeWindow){if let Some(u)=ui.get_users().row_data(ui.get_user_list_index().max(0) as usize){open_user(ui,u,26);}}
fn open_avatar_picker(ui:&HomeWindow){let id=ui.get_profile_edit_user_id().to_string();let weak=ui.as_weak();thread::spawn(move||if let Ok(r)=users_command(&format!("LIST_AVATARS\t{}",id)){let mut raw=Vec::new();for l in r.lines(){if let Some(v)=l.strip_prefix("avatar="){let f:Vec<&str>=v.split('\t').collect();if f.len()>=3{raw.push((f[0].to_owned(),f[1].to_owned(),f[2].to_owned()));}}}let _=slint::invoke_from_event_loop(move||if let Some(ui)=weak.upgrade(){let rows:Vec<AvatarChoiceEntry>=raw.into_iter().map(|(spec,label,path)|{let lab=if let Some(n)=spec.strip_prefix("builtin:"){tr_arg(&ui,314,"Avatar {0}",n)}else{label};AvatarChoiceEntry{spec:spec.into(),label:lab.into(),path:path.clone().into(),avatar:avatar_image(&path)}}).collect();ui.set_avatar_choices(ModelRc::from(Rc::new(VecModel::from(rows))));ui.set_avatar_picker_index(0);ui.set_avatar_picker_scroll(0);ui.set_avatar_picker_open(true);});});}
fn timezone_region(zone:&str)->String{
    if zone=="UTC"{return "UTC".into();}
    zone.split_once('/').map(|(r,_)|r.to_owned()).unwrap_or_else(||zone.to_owned())
}
fn timezone_remainder(zone:&str)->String{
    if zone=="UTC"{return "UTC".into();}
    zone.split_once('/').map(|(_,r)|r.replace('_'," ").replace('/'," / ")).unwrap_or_else(||zone.replace('_'," "))
}
fn timezone_region_rank(region:&str)->usize{
    const ORDER:[&str;10]=["Europe","America","Asia","Africa","Australia","Pacific","Atlantic","Indian","Antarctica","UTC"];
    ORDER.iter().position(|v|*v==region).unwrap_or(ORDER.len())
}
fn open_timezone_region(ui:&HomeWindow){
    let cur=ui.get_timezone().to_string();
    let w=ui.as_weak();
    thread::spawn(move||if let Ok(r)=regional_command("LIST_TIMEZONES"){
        let mut regions:Vec<String>=r.lines()
            .filter_map(|l|l.strip_prefix("zone="))
            .map(timezone_region)
            .collect();
        regions.sort_by(|a,b|{
            let ar=timezone_region_rank(a);let br=timezone_region_rank(b);
            ar.cmp(&br).then_with(||a.to_lowercase().cmp(&b.to_lowercase()))
        });
        regions.dedup();
        let current_region=timezone_region(&cur);
        let options=regions.into_iter().map(|r|(r.clone(),r)).collect();
        let _=slint::invoke_from_event_loop(move||if let Some(ui)=w.upgrade(){
            open_settings_choice(&ui,"timezone-region",&tr(&ui,280,"Timezone"),options,&current_region);
        });
    });
}
fn open_timezone_city(ui:&HomeWindow,region:String){
    let cur=ui.get_timezone().to_string();
    let w=ui.as_weak();
    thread::spawn(move||if let Ok(r)=regional_command("LIST_TIMEZONES"){
        let mut options:Vec<(String,String)>=r.lines()
            .filter_map(|l|l.strip_prefix("zone="))
            .filter(|z|timezone_region(z)==region)
            .map(|z|(z.to_owned(),timezone_remainder(z)))
            .collect();
        options.sort_by(|a,b|a.1.to_lowercase().cmp(&b.1.to_lowercase()));
        let _=slint::invoke_from_event_loop(move||if let Some(ui)=w.upgrade(){
            let title=format!("{} • {}",tr(&ui,280,"Timezone"),region);
            open_settings_choice(&ui,"timezone-city",&title,options,&cur);
        });
    });
}
fn open_keyboard_choice(ui:&HomeWindow){let cur=ui.get_keyboard_layout_code().to_string();let w=ui.as_weak();thread::spawn(move||if let Ok(r)=regional_command("LIST_KEYBOARDS"){let mut o=Vec::new();for l in r.lines(){if let Some(v)=l.strip_prefix("keyboard="){let f:Vec<&str>=v.split('\t').collect();if f.len()>=2{o.push((f[0].into(),f[1].into()));}}}let _=slint::invoke_from_event_loop(move||if let Some(ui)=w.upgrade(){open_settings_choice(&ui,"keyboard-layout",&tr(&ui,283,"Keyboard Layout"),o,&cur);});});}
fn open_startup(ui:&HomeWindow){open_settings_choice(ui,"user-login-mode",&tr(ui,276,"Startup"),vec![("select".into(),tr(ui,277,"User Picker")),("default".into(),tr(ui,278,"Direct Login"))],ui.get_user_login_mode().as_str());}fn open_default_user(ui:&HomeWindow){let mut o=Vec::new();for i in 0..ui.get_users().row_count(){if let Some(u)=ui.get_users().row_data(i){o.push((u.id.to_string(),u.name.to_string()));}}open_settings_choice(ui,"default-user",&tr(ui,324,"Boot User"),o,ui.get_default_user_id().as_str());}
fn write_ui_context(c:&str){let _=fs::create_dir_all("/run/nuubos");let _=fs::write(UI_CONTEXT,format!("{}\n",c));}

fn parse_system_snapshot(reply: &str) -> SystemSnapshot {
    let mut s = SystemSnapshot::default();
    for line in reply.lines() {
        let Some((key, value)) = line.split_once('=') else {
            continue;
        };
        match key {
            "nuubos.version" => s.version = value.to_owned(),
            "nuubos.build" => s.build = value.to_owned(),
            "profile.requested" => s.profile = value.to_owned(),
            "profile.effective" => s.effective_profile = value.to_owned(),
            "auto_battery.threshold" => s.auto_battery_threshold = value.parse().unwrap_or(0),
            "screensaver_after_min" => s.screensaver_after_min = value.parse().unwrap_or(0),
            "sleep_after_min" => s.sleep_after_min = value.parse().unwrap_or(0),
            "poweroff_after_min" => s.poweroff_after_min = value.parse().unwrap_or(0),
            "rtc_wakeup" => s.rtc_wakeup = value == "1",
            "cpu.freq_khz" => s.cpu_freq_khz = value.parse().unwrap_or(-1),
            "gpu.freq_hz" => s.gpu_freq_hz = value.parse().unwrap_or(-1),
            "temp.cpu_millic" => s.cpu_temp_millic = value.parse().unwrap_or(-1),
            "temp.gpu_millic" => s.gpu_temp_millic = value.parse().unwrap_or(-1),
            "mem.total_kib" => s.mem_total_kib = value.parse().unwrap_or(-1),
            "mem.available_kib" => s.mem_available_kib = value.parse().unwrap_or(-1),
            "uptime_sec" => s.uptime_sec = value.parse().unwrap_or(-1),
            "device.model" => s.device_model = value.to_owned(),
            "kernel" => s.kernel = value.to_owned(),
            "network" => s.network = value.to_owned(),
            "storage.mode" => s.storage_mode = value.to_owned(),
            "storage.active" => s.storage_active = value.to_owned(),
            "storage.health" => s.storage_health = value.to_owned(),
            "storage.tf2_state" => s.tf2_state = value.to_owned(),
            "storage.tf2_cid" => s.tf2_cid = value.to_owned(),
            "storage.action_required" => s.storage_action_required = value.to_owned(),
            "storage.backup_policy" => s.backup_policy = value.to_owned(),
            "storage.backup_due" => s.backup_due = value.to_owned(),
            "storage.backup_reason" => s.backup_reason = value.to_owned(),
            "storage.job_state" => s.storage_job_state = value.to_owned(),
            "storage.job_action" => s.storage_job_action = value.to_owned(),
            "storage.job_exit" => s.storage_job_exit = value.parse().unwrap_or(0),
            "storage.userdata_total" => s.userdata_total = value.parse().unwrap_or(0),
            "storage.userdata_free" => s.userdata_free = value.parse().unwrap_or(0),
            "lifecycle.stage" => s.idle_stage = value.to_owned(),
            _ => {}
        }
    }
    s
}

fn human_bytes(value: u64) -> String {
    if value >= 1024 * 1024 * 1024 {
        format!("{:.1} GiB", value as f64 / 1024.0 / 1024.0 / 1024.0)
    } else if value >= 1024 * 1024 {
        format!("{:.0} MiB", value as f64 / 1024.0 / 1024.0)
    } else {
        format!("{} KiB", value / 1024)
    }
}

fn apply_system_snapshot(ui: &HomeWindow, s: &SystemSnapshot) {
    ui.set_system_version(s.version.clone().into());
    ui.set_system_build(s.build.clone().into());
    ui.set_system_profile(s.profile.clone().into());
    ui.set_system_effective_profile(s.effective_profile.clone().into());
    ui.set_system_auto_battery_threshold(s.auto_battery_threshold);
    ui.set_system_screensaver_after_min(s.screensaver_after_min);
    ui.set_system_sleep_after_min(s.sleep_after_min);
    ui.set_system_poweroff_after_min(s.poweroff_after_min);
    ui.set_system_rtc_wakeup(s.rtc_wakeup);

    ui.set_system_cpu_clock(if s.cpu_freq_khz >= 0 {
        format!("{:.2} GHz", s.cpu_freq_khz as f64 / 1_000_000.0).into()
    } else { "Unavailable".into() });
    ui.set_system_gpu_clock(if s.gpu_freq_hz >= 0 {
        format!("{} MHz", s.gpu_freq_hz / 1_000_000).into()
    } else { "Unavailable".into() });
    ui.set_system_cpu_temperature(if s.cpu_temp_millic >= 0 {
        format!("{:.1} °C", s.cpu_temp_millic as f64 / 1000.0).into()
    } else { "Unavailable".into() });
    ui.set_system_gpu_temperature(if s.gpu_temp_millic >= 0 {
        format!("{:.1} °C", s.gpu_temp_millic as f64 / 1000.0).into()
    } else { "Unavailable".into() });
    if s.mem_total_kib > 0 && s.mem_available_kib >= 0 {
        let used_kib = s.mem_total_kib.saturating_sub(s.mem_available_kib);
        ui.set_system_memory(format!(
            "{} used • {} available",
            human_bytes((used_kib as u64) * 1024),
            human_bytes((s.mem_available_kib as u64) * 1024)
        ).into());
    } else {
        ui.set_system_memory(tr(ui, 241, "Unavailable").into());
    }

    ui.set_screensaver_active(s.idle_stage == "screensaver");

    ui.set_system_uptime(if s.uptime_sec >= 0 {
        let hours = s.uptime_sec / 3600;
        let minutes = (s.uptime_sec % 3600) / 60;
        format!("{}h {}m", hours, minutes).into()
    } else { "Unavailable".into() });

    ui.set_system_device_model(s.device_model.clone().into());
    ui.set_system_kernel(s.kernel.clone().into());
    ui.set_system_network(s.network.clone().into());
    ui.set_system_storage_mode(s.storage_mode.clone().into());
    ui.set_system_storage_active(s.storage_active.clone().into());
    ui.set_system_storage_health(s.storage_health.clone().into());
    ui.set_system_tf2_state(s.tf2_state.clone().into());
    ui.set_system_tf2_cid(s.tf2_cid.clone().into());
    ui.set_system_storage_action_required(s.storage_action_required.clone().into());
    ui.set_system_backup_policy(s.backup_policy.clone().into());
    ui.set_system_backup_due(s.backup_due.clone().into());
    ui.set_system_backup_reason(s.backup_reason.clone().into());
    ui.set_system_storage_job_state(s.storage_job_state.clone().into());
    ui.set_system_storage_job_action(s.storage_job_action.clone().into());
    ui.set_system_storage_job_exit(s.storage_job_exit);

    if s.userdata_total > 0 {
        let used = s.userdata_total.saturating_sub(s.userdata_free);
        ui.set_system_storage_usage(
            format!("{} used of {}", human_bytes(used), human_bytes(s.userdata_total)).into()
        );
    } else {
        ui.set_system_storage_usage(tr(ui, 241, "Unavailable").into());
    }
}

fn refresh_system(ui: &HomeWindow) {
    let weak = ui.as_weak();
    thread::spawn(move || {
        let snapshot = match system_command("STATUS") {
            Ok(reply) => Some(parse_system_snapshot(&reply)),
            Err(error) => {
                eprintln!("home: System snapshot failed={error}");
                None
            }
        };

        let _ = slint::invoke_from_event_loop(move || {
            if let (Some(ui), Some(snapshot)) = (weak.upgrade(), snapshot) {
                apply_system_snapshot(&ui, &snapshot);
            }
        });
    });
}

fn system_set(ui: &HomeWindow, command: String) {
    let weak = ui.as_weak();
    thread::spawn(move || {
        if let Err(error) = system_command(&command) {
            eprintln!("home: System command {:?} failed={error}", command);
        }

        let snapshot = match system_command("STATUS") {
            Ok(reply) => Some(parse_system_snapshot(&reply)),
            Err(error) => {
                eprintln!("home: System refresh after {:?} failed={error}", command);
                None
            }
        };

        let _ = slint::invoke_from_event_loop(move || {
            if let (Some(ui), Some(snapshot)) = (weak.upgrade(), snapshot) {
                apply_system_snapshot(&ui, &snapshot);
            }
        });
    });
}

fn start_system_listener(ui: &HomeWindow, screensaver_gate: Arc<(Mutex<bool>, Condvar)>) {
    let weak = ui.as_weak();
    thread::spawn(move || loop {
        match UnixStream::connect(SYSTEM_SOCKET) {
            Ok(mut stream) => {
                if stream.write_all(b"SUBSCRIBE\n").is_err() {
                    thread::sleep(Duration::from_millis(500));
                    continue;
                }
                let mut reader = BufReader::new(stream);
                let mut line = String::new();
                loop {
                    line.clear();
                    match reader.read_line(&mut line) {
                        Ok(0) => break,
                        Ok(_) => {
                            if line.trim() != "changed" && !line.starts_with("OK") {
                                continue;
                            }
                            let snapshot = system_command("STATUS")
                                .map(|reply| parse_system_snapshot(&reply));
                            if let Ok(snapshot) = snapshot {
                                {
                                    let (lock, cv) = &*screensaver_gate;
                                    let mut active = lock.lock().unwrap();
                                    let next = snapshot.idle_stage == "screensaver";
                                    let changed = *active != next;
                                    *active = next;
                                    if changed && next { cv.notify_one(); }
                                }
                                let weak = weak.clone();
                                let _ = slint::invoke_from_event_loop(move || {
                                    if let Some(ui) = weak.upgrade() {
                                        apply_system_snapshot(&ui, &snapshot);
                                    }
                                });
                            }
                        }
                        Err(_) => break,
                    }
                }
            }
            Err(_) => {}
        }
        thread::sleep(Duration::from_millis(500));
    });
}

#[derive(Clone, Debug)]
struct AudioProductState {
    mode: String,
    selected: String,
    volume: i32,
    volume_supported: bool,
    bluetooth_available: bool,
    headphones_available: bool,
    hdmi_available: bool,
    speaker_available: bool,
    system_volume: i32,
    home_music_volume: i32,
    home_music_playing: bool,
}

impl Default for AudioProductState {
    fn default() -> Self {
        Self {
            mode: "auto".to_owned(),
            selected: "speaker".to_owned(),
            volume: 100,
            volume_supported: true,
            bluetooth_available: false,
            headphones_available: false,
            hdmi_available: false,
            speaker_available: true,
            system_volume: 60,
            home_music_volume: 60,
            home_music_playing: false,
        }
    }
}

fn audio_output_label(ui: &HomeWindow, output: &str) -> String {
    match output {
        "auto" => tr(ui, 122, "Automatic"),
        "bluetooth" => tr(ui, 331, "Bluetooth Audio"),
        "analog" => tr(ui, 332, "Analog Audio"),
        "headphones" => tr(ui, 330, "Headphones"),
        "hdmi" => "HDMI".to_owned(),
        "speaker" => tr(ui, 240, "Speaker"),
        _ => tr(ui, 211, "Unknown"),
    }
}

fn apply_audio_product_state(ui: &HomeWindow, state: AudioProductState) {
    let selected_label = audio_output_label(ui, state.selected.as_str());
    let mode_label = if state.mode == "auto" {
        format!("Automatic • {selected_label}")
    } else if state.mode == "analog" {
        format!("Analog • {selected_label}")
    } else {
        audio_output_label(ui, state.mode.as_str())
    };

    ui.set_audio_mode(state.mode.clone().into());
    ui.set_audio_selected(state.selected.clone().into());
    ui.set_audio_output_label(mode_label.into());
    ui.set_audio_volume(state.volume);
    ui.set_audio_volume_supported(state.volume_supported);
    ui.set_audio_bluetooth_available(state.bluetooth_available);
    ui.set_audio_headphones_available(state.headphones_available);
    ui.set_audio_hdmi_available(state.hdmi_available);
    ui.set_audio_speaker_available(state.speaker_available);
    ui.set_system_sounds_volume(state.system_volume);
    ui.set_home_music_volume(state.home_music_volume);

    /* Display capability is owned by the display path. Audio StateChanged
     * events must never overwrite the internal-panel availability flag. */
}

fn apply_wifi_product_state(ui: &HomeWindow, state: WifiProductState) {
    let status = match state.state.as_str() {
        "connected" => "Connected",
        "connecting" => "Connecting",
        "off" => "Off",
        "disconnected" => "Disconnected",
        _ => "Unavailable",
    };

    let detail = if state.state == "connected" {
        let mut parts = Vec::new();
        if !state.ssid.is_empty() {
            parts.push(state.ssid.clone());
        }
        if !state.ipv4.is_empty() {
            parts.push(state.ipv4.clone());
        }
        if state.signal_dbm != 0 {
            parts.push(format!("{} dBm", state.signal_dbm));
        }
        if parts.is_empty() {
            "Connected".to_owned()
        } else {
            parts.join("  •  ")
        }
    } else if state.state == "connecting" {
        if state.ssid.is_empty() {
            "Association in progress".to_owned()
        } else {
            format!("Connecting to {}", state.ssid)
        }
    } else if state.state == "off" || !state.enabled {
        "Wireless radio is disabled".to_owned()
    } else if state.state == "disconnected" {
        "No network connected".to_owned()
    } else {
        "Wi-Fi product service unavailable".to_owned()
    };

    ui.set_connectivity_wifi_status(status.into());
    ui.set_connectivity_wifi_detail(detail.into());
    ui.set_connectivity_wifi_active(state.state == "connected");
    ui.set_wifi_enabled(state.enabled);
    ui.set_wifi_current_ssid(state.ssid.clone().into());

    let selected_ssid = ui.get_wifi_selected_ssid().to_string();
    ui.set_wifi_selected_current(
        state.state == "connected"
            && !selected_ssid.is_empty()
            && selected_ssid == state.ssid
    );
}

fn apply_bluetooth_product_state(ui: &HomeWindow, state: BluetoothProductState) {
    let status = if !state.present {
        "Unavailable"
    } else if state.powered {
        "On"
    } else {
        "Off"
    };

    let detail = if !state.present {
        "Bluetooth adapter not available".to_owned()
    } else if !state.powered {
        "Bluetooth radio is disabled".to_owned()
    } else if state.connected_count > 0 {
        format!(
            "{} connected  •  {} paired",
            state.connected_count, state.paired_count
        )
    } else if state.paired_count > 0 {
        format!("No device connected  •  {} paired", state.paired_count)
    } else if state.address.is_empty() {
        "Ready for devices".to_owned()
    } else {
        format!("Ready  •  {}", state.address)
    };

    ui.set_connectivity_bluetooth_status(status.into());
    ui.set_connectivity_bluetooth_detail(detail.into());
    ui.set_connectivity_bluetooth_active(state.present && state.powered);
    ui.set_bluetooth_enabled(state.present && state.powered);
}

fn wifi_proxy(connection: &zbus::blocking::Connection) -> zbus::Result<zbus::blocking::Proxy<'_>> {
    zbus::blocking::Proxy::new(
        connection,
        "org.nuubOS.Wifi",
        "/org/nuubOS/Wifi",
        "org.nuubOS.Wifi1",
    )
}

fn bluetooth_proxy(connection: &zbus::blocking::Connection) -> zbus::Result<zbus::blocking::Proxy<'_>> {
    zbus::blocking::Proxy::new(
        connection,
        "org.nuubOS.Bluetooth",
        "/org/nuubOS/Bluetooth",
        "org.nuubOS.Bluetooth1",
    )
}

fn controllers_proxy(connection: &zbus::blocking::Connection) -> zbus::Result<zbus::blocking::Proxy<'_>> {
    zbus::blocking::Proxy::new(
        connection,
        "org.nuubOS.Controllers",
        "/org/nuubOS/Controllers",
        "org.nuubOS.Controllers1",
    )
}

fn rumble_proxy(connection: &zbus::blocking::Connection) -> zbus::Result<zbus::blocking::Proxy<'_>> {
    zbus::blocking::Proxy::new(
        connection,
        "org.nuubOS.Rumble",
        "/org/nuubOS/Rumble",
        "org.nuubOS.Rumble1",
    )
}

fn lighting_proxy(connection: &zbus::blocking::Connection) -> zbus::Result<zbus::blocking::Proxy<'_>> {
    zbus::blocking::Proxy::new(
        connection,
        "org.nuubOS.Lighting",
        "/org/nuubOS/Lighting",
        "org.nuubOS.Lighting1",
    )
}

fn audio_proxy(connection: &zbus::blocking::Connection) -> zbus::Result<zbus::blocking::Proxy<'_>> {
    zbus::blocking::Proxy::new(
        connection,
        "org.nuubOS.Audio",
        "/org/nuubOS/Audio",
        "org.nuubOS.Audio1",
    )
}

fn string_model(values: &[&str]) -> ModelRc<SharedString> {
    let values: Vec<SharedString> = values.iter().map(|value| (*value).into()).collect();
    ModelRc::from(Rc::new(VecModel::from(values)))
}

fn apply_keyboard_layout(ui: &HomeWindow) {
    let page = ui.get_keyboard_page();
    let kind = ui.get_keyboard_input_kind().to_string();
    let layout = ui.get_keyboard_layout().to_string();

    let rows: [Vec<&str>; 4] = match kind.as_str() {
        "number" => [
            vec!["1", "2", "3"],
            vec!["4", "5", "6"],
            vec!["7", "8", "9"],
            vec!["Back", "0", "Done"],
        ],
        "ipv4" => [
            vec!["1", "2", "3"],
            vec!["4", "5", "6"],
            vec!["7", "8", "9"],
            vec![".", "0", "Back", "Done"],
        ],
        "dns" => [
            vec!["1", "2", "3"],
            vec!["4", "5", "6"],
            vec!["7", "8", "9"],
            vec![".", "0", ",", "Space", "Back", "Done"],
        ],
        _ => match page {
            1 => [
                vec!["1", "2", "3", "4", "5", "6", "7", "8", "9", "0"],
                vec!["!", "?", "#", "$", "%", "&", "*", "(", ")", "Back"],
                vec!["+", "=", "[", "]", "{", "}", "<", ">", "\\", "|"],
                vec!["ABC", "SYM", ".", ",", "-", "_", ":", ";", "Space", "Done"],
            ],
            2 => [
                vec!["`", "~", "^", "@", "€", "£", "¥", "¢", "°", "§"],
                vec![":", ";", "'", "\"", "\\", "/", "?", "!", "#", "Back"],
                vec!["_", "-", "+", "=", "[", "]", "{", "}", "(", ")"],
                vec!["ABC", "123", ".", ",", "<", ">", "*", "&", "Space", "Done"],
            ],
            _ if layout == "azerty" => [
                vec!["a", "z", "e", "r", "t", "y", "u", "i", "o", "p"],
                vec!["q", "s", "d", "f", "g", "h", "j", "k", "l", "m"],
                vec!["w", "x", "c", "v", "b", "n", ".", "-", "_", "Back"],
                vec!["Shift", "123", "SYM", "@", "/", ":", ";", "'", "Space", "Done"],
            ],
            _ if layout == "qwertz" => [
                vec!["q", "w", "e", "r", "t", "z", "u", "i", "o", "p"],
                vec!["a", "s", "d", "f", "g", "h", "j", "k", "l", "Back"],
                vec!["y", "x", "c", "v", "b", "n", "m", ".", "-", "_"],
                vec!["Shift", "123", "SYM", "@", "/", ":", ";", "'", "Space", "Done"],
            ],
            _ => [
                vec!["q", "w", "e", "r", "t", "y", "u", "i", "o", "p"],
                vec!["a", "s", "d", "f", "g", "h", "j", "k", "l", "Back"],
                vec!["z", "x", "c", "v", "b", "n", "m", ".", "-", "_"],
                vec!["Shift", "123", "SYM", "@", "/", ":", ";", "'", "Space", "Done"],
            ],
        },
    };

    let shift = ui.get_keyboard_shift() && page == 0 && kind == "text";
    let display_rows: [Vec<String>; 4] = rows.map(|row| {
        row.into_iter()
            .map(|key| {
                if shift && key.chars().count() == 1 && key.chars().all(|c| c.is_alphabetic()) {
                    key.to_uppercase()
                } else {
                    key.to_owned()
                }
            })
            .collect()
    });
    let model = |values: &[String]| {
        let values: Vec<SharedString> = values.iter().map(|value| value.as_str().into()).collect();
        ModelRc::from(Rc::new(VecModel::from(values)))
    };
    ui.set_keyboard_row_zero(model(&display_rows[0]));
    ui.set_keyboard_row_one(model(&display_rows[1]));
    ui.set_keyboard_row_two(model(&display_rows[2]));
    ui.set_keyboard_row_three(model(&display_rows[3]));
}

fn apply_wifi_network_rows(ui: &HomeWindow, rows: Vec<(String, String, i32, bool, bool)>, saved: bool) {
    let entries: Vec<WifiNetworkEntry> = rows
        .into_iter()
        .map(|(ssid, security, signal_dbm, is_saved, current)| WifiNetworkEntry {
            ssid: ssid.into(),
            security: security.into(),
            signal_dbm,
            saved: is_saved,
            current,
        })
        .collect();
    let model = ModelRc::from(Rc::new(VecModel::from(entries)));
    if saved {
        ui.set_wifi_saved_networks(model);
        let count = ui.get_wifi_saved_networks().row_count() as i32;
        if count == 0 {
            ui.set_wifi_saved_index(0);
            ui.set_wifi_saved_scroll_offset(0);
        } else if ui.get_wifi_saved_index() >= count {
            ui.set_wifi_saved_index(count - 1);
        }
    } else {
        ui.set_wifi_networks(model);
        let count = ui.get_wifi_networks().row_count() as i32;
        if count == 0 {
            ui.set_wifi_network_index(0);
            ui.set_wifi_network_scroll_offset(0);
        } else if ui.get_wifi_network_index() >= count {
            ui.set_wifi_network_index(count - 1);
        }

        let total = 5 + count;
        if ui.get_wifi_menu_index() >= total {
            ui.set_wifi_menu_index((total - 1).max(0));
        }
        update_wifi_menu_scroll(ui);
    }
}

fn refresh_wifi_networks(ui: &HomeWindow, saved: bool) {
    let weak = ui.as_weak();
    thread::spawn(move || {
        let result = (|| -> zbus::Result<Vec<(String, String, i32, bool, bool)>> {
            let connection = zbus::blocking::Connection::system()?;
            let proxy = wifi_proxy(&connection)?;
            proxy.call(if saved { "GetSavedNetworks" } else { "GetNetworks" }, &())
        })();
        match result {
            Ok(rows) => {
                let _ = slint::invoke_from_event_loop(move || {
                    if let Some(ui) = weak.upgrade() {
                        apply_wifi_network_rows(&ui, rows, saved);
                    }
                });
            }
            Err(error) => {
                eprintln!("home: Wi-Fi network list failed={error}");
            }
        }
    });
}


fn apply_bluetooth_device_rows(
    ui: &HomeWindow,
    rows: Vec<(String, String, String, bool, bool, bool, i32)>,
) {
    let mut nearby = Vec::new();
    let mut known = Vec::new();
    let selected = ui.get_bluetooth_selected_address().to_string();
    let mut selected_found = false;

    for (address, name, kind, paired, connected, trusted, rssi) in rows {
        let entry = BluetoothDeviceEntry {
            address: address.clone().into(),
            name: name.clone().into(),
            kind: kind.clone().into(),
            paired,
            connected,
            trusted,
            rssi,
        };

        if paired || connected {
            known.push(entry);
        } else {
            nearby.push(entry);
        }

        if !selected.is_empty() && selected == address {
            selected_found = true;
            ui.set_bluetooth_selected_name(name.into());
            ui.set_bluetooth_selected_kind(kind.into());
            ui.set_bluetooth_selected_paired(paired);
            ui.set_bluetooth_selected_connected(connected);
            ui.set_bluetooth_selected_trusted(trusted);
        }
    }

    if !selected.is_empty() && !selected_found {
        ui.set_bluetooth_selected_paired(false);
        ui.set_bluetooth_selected_connected(false);
        ui.set_bluetooth_selected_trusted(false);
    }

    ui.set_bluetooth_devices(ModelRc::from(Rc::new(VecModel::from(nearby))));
    ui.set_bluetooth_known_devices(ModelRc::from(Rc::new(VecModel::from(known))));

    let nearby_count = ui.get_bluetooth_devices().row_count() as i32;
    if nearby_count == 0 {
        ui.set_bluetooth_device_index(0);
        ui.set_bluetooth_device_scroll_offset(0);
    } else if ui.get_bluetooth_device_index() >= nearby_count {
        ui.set_bluetooth_device_index(nearby_count - 1);
    }

    let known_count = ui.get_bluetooth_known_devices().row_count() as i32;
    if known_count == 0 {
        ui.set_bluetooth_known_index(0);
        ui.set_bluetooth_known_scroll_offset(0);
    } else if ui.get_bluetooth_known_index() >= known_count {
        ui.set_bluetooth_known_index(known_count - 1);
    }
}

fn refresh_bluetooth_devices(ui: &HomeWindow) {
    let weak = ui.as_weak();
    thread::spawn(move || {
        let result = (|| -> zbus::Result<Vec<(String, String, String, bool, bool, bool, i32)>> {
            let connection = zbus::blocking::Connection::system()?;
            let proxy = bluetooth_proxy(&connection)?;
            proxy.call("GetDevices", &())
        })();

        match result {
            Ok(rows) => {
                let _ = slint::invoke_from_event_loop(move || {
                    if let Some(ui) = weak.upgrade() {
                        apply_bluetooth_device_rows(&ui, rows);
                    }
                });
            }
            Err(error) => eprintln!("home: Bluetooth device list failed={error}"),
        }
    });
}

fn bluetooth_notice(ui: &HomeWindow, text: impl Into<SharedString>) {
    ui.set_bluetooth_notice(text.into());
}

fn bluetooth_set_enabled(ui: &HomeWindow, enabled: bool) {
    let weak = ui.as_weak();
    bluetooth_notice(ui, if enabled { tr(ui, 242, "Enabling Bluetooth…") } else { tr(ui, 243, "Disabling Bluetooth…") });
    thread::spawn(move || {
        let result = (|| -> zbus::Result<()> {
            let connection = zbus::blocking::Connection::system()?;
            let proxy = bluetooth_proxy(&connection)?;
            proxy.call("SetEnabled", &(enabled,))
        })();

        let _ = slint::invoke_from_event_loop(move || {
            if let Some(ui) = weak.upgrade() {
                match result {
                    Ok(()) => {
                        bluetooth_notice(&ui, "");
                        refresh_bluetooth_devices(&ui);
                    }
                    Err(error) => bluetooth_notice(&ui, tr_arg(&ui, 244, "Bluetooth toggle failed: {0}", &error.to_string())),
                }
            }
        });
    });
}

fn bluetooth_start_discovery_session(ui: &HomeWindow) {
    let weak = ui.as_weak();
    ui.set_bluetooth_scanning(true);
    bluetooth_notice(ui, "");
    thread::spawn(move || {
        let result = (|| -> zbus::Result<()> {
            let connection = zbus::blocking::Connection::system()?;
            let proxy = bluetooth_proxy(&connection)?;
            proxy.call("StartDiscoverySession", &())
        })();

        if let Err(error) = result {
            let _ = slint::invoke_from_event_loop(move || {
                if let Some(ui) = weak.upgrade() {
                    ui.set_bluetooth_scanning(false);
                    bluetooth_notice(&ui, tr_arg(&ui, 245, "Discovery failed: {0}", &error.to_string()));
                }
            });
        }
    });
}

fn bluetooth_stop_discovery_session(ui: &HomeWindow) {
    ui.set_bluetooth_scanning(false);
    thread::spawn(move || {
        let _ = (|| -> zbus::Result<()> {
            let connection = zbus::blocking::Connection::system()?;
            let proxy = bluetooth_proxy(&connection)?;
            proxy.call("StopDiscoverySession", &())
        })();
    });
}

fn bluetooth_device_operation(ui: &HomeWindow, method: &'static str, address: String) {
    let weak = ui.as_weak();
    let progress = match method {
        "PairAndConnect" => "Pairing and connecting…",
        "Connect" => "Connecting…",
        "Disconnect" => "Disconnecting…",
        "Forget" => "Forgetting device…",
        _ => "Working…",
    };
    bluetooth_notice(ui, progress);

    thread::spawn(move || {
        let result = (|| -> zbus::Result<()> {
            let connection = zbus::blocking::Connection::system()?;
            let proxy = bluetooth_proxy(&connection)?;
            proxy.call(method, &(address.as_str(),))
        })();

        let _ = slint::invoke_from_event_loop(move || {
            if let Some(ui) = weak.upgrade() {
                match result {
                    Ok(()) => {
                        bluetooth_notice(&ui, "");
                        refresh_bluetooth_devices(&ui);
                        if method == "Forget" {
                            ui.set_bluetooth_selected_paired(false);
                            ui.set_bluetooth_selected_connected(false);
                            navigate_settings_view(&ui, 8);
                        }
                    }
                    Err(error) => bluetooth_notice(&ui, tr_arg(&ui, 246, "Bluetooth operation failed: {0}", &error.to_string())),
                }
            }
        });
    });
}

fn bluetooth_pair_with_pin(ui: &HomeWindow, address: String, pin: String) {
    let weak = ui.as_weak();
    bluetooth_notice(ui, tr(ui, 247, "Pairing with PIN…"));

    thread::spawn(move || {
        let result = (|| -> zbus::Result<()> {
            let connection = zbus::blocking::Connection::system()?;
            let proxy = bluetooth_proxy(&connection)?;
            proxy.call("PairWithPin", &(address.as_str(), pin.as_str()))
        })();

        let _ = slint::invoke_from_event_loop(move || {
            if let Some(ui) = weak.upgrade() {
                match result {
                    Ok(()) => {
                        bluetooth_notice(&ui, "");
                        refresh_bluetooth_devices(&ui);
                    }
                    Err(error) => bluetooth_notice(&ui, tr_arg(&ui, 248, "Bluetooth PIN pairing failed: {0}", &error.to_string())),
                }
            }
        });
    });
}


fn apply_controller_rows(
    ui: &HomeWindow,
    rows: Vec<(String, String, String, bool, bool, i32, i32, i32)>,
) {
    /* Top-bar controllers and their battery come from the typed Controllers
     * Product Service: every connected external controller holding a player
     * slot, in player order. */
    let mut topbar: Vec<TopbarController> = rows
        .iter()
        .filter(|(_, _, _, connected, builtin, _, player, _)| *connected && !*builtin && *player > 0)
        .map(|(_, _, _, _, _, _, player, battery)| TopbarController {
            player: *player,
            battery: *battery,
        })
        .collect();
    topbar.sort_by_key(|controller| controller.player);
    ui.set_topbar_controllers(ModelRc::from(Rc::new(VecModel::from(topbar))));

    let entries: Vec<ControllerEntry> = rows
        .into_iter()
        .map(
            |(id, name, transport, connected, builtin, preferred_player, effective_player, _battery)| {
                ControllerEntry {
                    id: id.into(),
                    name: name.into(),
                    transport: transport.into(),
                    connected,
                    builtin,
                    preferred_player,
                    effective_player,
                }
            },
        )
        .collect();

    ui.set_controllers(ModelRc::from(Rc::new(VecModel::from(entries))));
    let count = ui.get_controllers().row_count() as i32;
    if count == 0 {
        ui.set_controller_list_index(0);
        ui.set_controller_list_scroll_offset(0);
    } else if ui.get_controller_list_index() >= count {
        ui.set_controller_list_index(count - 1);
    }
    update_controller_list_scroll(ui);
}

fn apply_player_assignment_rows(
    ui: &HomeWindow,
    rows: Vec<(i32, String, String, bool)>,
) {
    let entries: Vec<PlayerAssignmentEntry> = rows
        .into_iter()
        .map(|(player, controller_id, controller_name, available)| PlayerAssignmentEntry {
            player,
            controller_id: controller_id.into(),
            controller_name: controller_name.into(),
            available,
        })
        .collect();

    ui.set_player_assignments(ModelRc::from(Rc::new(VecModel::from(entries))));
    let count = ui.get_player_assignments().row_count() as i32;
    if count > 0 && ui.get_player_assignment_index() >= count {
        ui.set_player_assignment_index(count - 1);
    }
    update_player_assignment_scroll(ui);
}

fn refresh_controllers(ui: &HomeWindow) {
    let weak = ui.as_weak();
    thread::spawn(move || {
        let result = (|| -> zbus::Result<(
            Vec<(String, String, String, bool, bool, i32, i32, i32)>,
            Vec<(i32, String, String, bool)>,
        )> {
            let connection = zbus::blocking::Connection::system()?;
            let proxy = controllers_proxy(&connection)?;
            let devices = proxy.call("GetDevices", &())?;
            let assignments = proxy.call("GetAssignments", &())?;
            Ok((devices, assignments))
        })();

        match result {
            Ok((devices, assignments)) => {
                let _ = slint::invoke_from_event_loop(move || {
                    if let Some(ui) = weak.upgrade() {
                        apply_controller_rows(&ui, devices);
                        apply_player_assignment_rows(&ui, assignments);
                    }
                });
            }
            Err(error) => eprintln!("home: Controllers refresh failed={error}"),
        }
    });
}

/// Localized name of a logical controller control (Controllers service ids).
fn control_label(ui: &HomeWindow, control: &str) -> String {
    let (index, fallback) = match control {
        "menu_back" => (337, "Face South"),
        "menu_confirm" => (338, "Face East"),
        "face_north" => (339, "Face North"),
        "face_west" => (340, "Face West"),
        "menu_up" => (341, "D-Pad Up"),
        "menu_down" => (342, "D-Pad Down"),
        "menu_left" => (343, "D-Pad Left"),
        "menu_right" => (344, "D-Pad Right"),
        "l1" => (345, "L1"),
        "r1" => (346, "R1"),
        "l2" => (347, "L2"),
        "r2" => (348, "R2"),
        "l3" => (349, "L3"),
        "r3" => (350, "R3"),
        "settings" => (351, "Start"),
        "select" => (352, "Select"),
        "quick_menu" => (353, "Hotkey"),
        "left_x" => (354, "Left Stick X"),
        "left_y" => (355, "Left Stick Y"),
        "right_x" => (356, "Right Stick X"),
        "right_y" => (357, "Right Stick Y"),
        "left_deadzone" => (123, "Left Stick Deadzone"),
        "right_deadzone" => (124, "Right Stick Deadzone"),
        _ => return control.to_owned(),
    };
    tr(ui, index, fallback)
}

/// Physical key: Linux gamepad codes carry a position semantic, so they share
/// the logical control names; anything else is a numbered button.
fn key_source_label(ui: &HomeWindow, code: i32) -> String {
    let control = match code {
        304 => "menu_back",
        305 => "menu_confirm",
        307 => "face_north",
        308 => "face_west",
        544 => "menu_up",
        545 => "menu_down",
        546 => "menu_left",
        547 => "menu_right",
        310 => "l1",
        311 => "r1",
        312 => "l2",
        313 => "r2",
        317 => "l3",
        318 => "r3",
        315 => "settings",
        314 => "select",
        316 => "quick_menu",
        _ => return tr_arg(ui, 358, "Button {0}", &code.to_string()),
    };
    control_label(ui, control)
}

fn abs_axis_name(code: i32) -> String {
    match code {
        0 => "X".to_owned(),
        1 => "Y".to_owned(),
        2 => "Z".to_owned(),
        3 => "RX".to_owned(),
        4 => "RY".to_owned(),
        5 => "RZ".to_owned(),
        6 => "THROTTLE".to_owned(),
        7 => "RUDDER".to_owned(),
        8 => "WHEEL".to_owned(),
        9 => "GAS".to_owned(),
        10 => "BRAKE".to_owned(),
        16..=23 => format!("HAT{}{}", (code - 16) / 2, if code % 2 == 0 { "X" } else { "Y" }),
        _ => code.to_string(),
    }
}

/// Mapping source as reported by the Controllers service:
/// key:CODE | abs:CODE:(+|-|>|<) | none.
fn source_label(ui: &HomeWindow, source: &str, axis_control: bool) -> String {
    if let Some(code) = source.strip_prefix("key:").and_then(|c| c.parse::<i32>().ok()) {
        return key_source_label(ui, code);
    }
    let Some((code, dir)) = source
        .strip_prefix("abs:")
        .and_then(|rest| rest.split_once(':'))
        .and_then(|(code, dir)| code.parse::<i32>().ok().map(|code| (code, dir)))
    else {
        return tr(ui, 361, "Not assigned");
    };
    if !axis_control {
        /* Hat 0 half axes are the D-pad directions. */
        match (code, dir) {
            (16, "-") => return control_label(ui, "menu_left"),
            (16, "+") => return control_label(ui, "menu_right"),
            (17, "-") => return control_label(ui, "menu_up"),
            (17, "+") => return control_label(ui, "menu_down"),
            _ => {}
        }
    }
    let axis = tr_arg(ui, 359, "Axis {0}", &abs_axis_name(code));
    match dir {
        "-" if axis_control => format!("{axis}  •  {}", tr(ui, 360, "Inverted")),
        "+" if !axis_control => format!("{axis} +"),
        "-" => format!("{axis} −"),
        _ => axis,
    }
}

fn lighting_color_name(r: i32, g: i32, b: i32) -> String {
    match (r, g, b) {
        (58, 134, 255) => "Blue".to_owned(),
        (171, 71, 188) => "Purple".to_owned(),
        (76, 175, 80) => "Green".to_owned(),
        (255, 152, 0) => "Orange".to_owned(),
        (244, 67, 54) => "Red".to_owned(),
        (0, 188, 212) => "Cyan".to_owned(),
        (255, 255, 255) => "White".to_owned(),
        (255, 193, 7) => "Amber".to_owned(),
        _ => "Custom".to_owned(),
    }
}

/// Input Tester line: logical control resolved by the Controllers service and
/// the physical source that produced it.
fn tester_display_label(ui: &HomeWindow, control: &str, event_type: i32, code: i32, value: i32) -> String {
    let logical = if control.is_empty() {
        tr(ui, 361, "Not assigned")
    } else {
        control_label(ui, control)
    };
    if event_type == 3 {
        let axis = tr_arg(ui, 359, "Axis {0}", &abs_axis_name(code));
        let percent = if control.ends_with("_x") || control.ends_with("_y") || control.is_empty() {
            format!("{value:+}%")
        } else {
            format!("{value}%")
        };
        return format!("{logical}  •  {axis} {percent}");
    }
    format!("{logical}  •  {}", key_source_label(ui, code))
}

/// Tester values arrive as logical values: buttons 0..100, stick axes -100..100.
fn set_tester_visual_state(ui: &HomeWindow, control: &str, value: i32) {
    let pressed = value >= 50;
    let axis = value.clamp(-100, 100);
    match control {
        "menu_back" => ui.set_tester_face_south(pressed),
        "menu_confirm" => ui.set_tester_face_east(pressed),
        "face_north" => ui.set_tester_face_north(pressed),
        "face_west" => ui.set_tester_face_west(pressed),
        "menu_up" => ui.set_tester_dpad_up(pressed),
        "menu_down" => ui.set_tester_dpad_down(pressed),
        "menu_left" => ui.set_tester_dpad_left(pressed),
        "menu_right" => ui.set_tester_dpad_right(pressed),
        "l1" => ui.set_tester_l1(pressed),
        "r1" => ui.set_tester_r1(pressed),
        "l2" => {
            ui.set_tester_l2(pressed);
            ui.set_tester_left_trigger(value.clamp(0, 100));
        }
        "r2" => {
            ui.set_tester_r2(pressed);
            ui.set_tester_right_trigger(value.clamp(0, 100));
        }
        "l3" => ui.set_tester_l3(pressed),
        "r3" => ui.set_tester_r3(pressed),
        "settings" => ui.set_tester_start(pressed),
        "select" => ui.set_tester_select(pressed),
        "quick_menu" => ui.set_tester_hotkey(pressed),
        "left_x" => ui.set_tester_left_x(axis),
        "left_y" => ui.set_tester_left_y(axis),
        "right_x" => ui.set_tester_right_x(axis),
        "right_y" => ui.set_tester_right_y(axis),
        _ => {}
    }
}

fn reset_tester_visual_state(ui: &HomeWindow) {
    ui.set_tester_face_south(false);
    ui.set_tester_face_east(false);
    ui.set_tester_face_north(false);
    ui.set_tester_face_west(false);
    ui.set_tester_dpad_up(false);
    ui.set_tester_dpad_down(false);
    ui.set_tester_dpad_left(false);
    ui.set_tester_dpad_right(false);
    ui.set_tester_l1(false);
    ui.set_tester_r1(false);
    ui.set_tester_l2(false);
    ui.set_tester_r2(false);
    ui.set_tester_l3(false);
    ui.set_tester_r3(false);
    ui.set_tester_start(false);
    ui.set_tester_select(false);
    ui.set_tester_hotkey(false);
    ui.set_tester_left_x(0);
    ui.set_tester_left_y(0);
    ui.set_tester_right_x(0);
    ui.set_tester_right_y(0);
    ui.set_tester_left_trigger(0);
    ui.set_tester_right_trigger(0);
}

/// Controller Mapping rows, one list: buttons, then each stick's axes
/// followed by its deadzone, then Restore Default Mapping.
/// kind: 0 button, 1 stick axis, 2 deadzone, 3 restore.
fn apply_mapping_rows(ui: &HomeWindow, rows: Vec<(String, i32, String, bool)>) {
    let mut entries: Vec<ControllerMappingEntry> = Vec::with_capacity(rows.len() + 3);
    for (control, kind, source, customized) in rows {
        entries.push(ControllerMappingEntry {
            label: control_label(ui, &control).into(),
            value: source_label(ui, &source, kind == 1).into(),
            control: control.as_str().into(),
            kind,
            customized,
        });
        let deadzone = match control.as_str() {
            "left_y" => "left_deadzone",
            "right_y" => "right_deadzone",
            _ => continue,
        };
        entries.push(ControllerMappingEntry {
            label: control_label(ui, deadzone).into(),
            value: "".into(),
            control: deadzone.into(),
            kind: 2,
            customized: false,
        });
    }
    entries.push(ControllerMappingEntry {
        label: tr(ui, 365, "Restore Default Mapping").into(),
        value: "".into(),
        control: "".into(),
        kind: 3,
        customized: false,
    });
    ui.set_controller_mapping(ModelRc::from(Rc::new(VecModel::from(entries))));
    let count = ui.get_controller_mapping().row_count() as i32;
    if ui.get_controller_mapping_index() >= count {
        ui.set_controller_mapping_index(count - 1);
    }
    update_controller_mapping_scroll(ui);
}

fn selected_mapping_entry(ui: &HomeWindow) -> Option<ControllerMappingEntry> {
    ui.get_controller_mapping()
        .row_data(ui.get_controller_mapping_index().max(0) as usize)
}

fn refresh_controller_mapping(ui: &HomeWindow) {
    let id = ui.get_controller_selected_id().to_string();
    if id.is_empty() {
        return;
    }

    let weak = ui.as_weak();
    thread::spawn(move || {
        let result = (|| -> zbus::Result<(Vec<(String, i32, String, bool)>, (i32, i32))> {
            let connection = zbus::blocking::Connection::system()?;
            let proxy = controllers_proxy(&connection)?;
            let rows: Vec<(String, i32, String, bool)> =
                proxy.call("GetMapping", &(id.as_str(),))?;
            let deadzones: (i32, i32) =
                proxy.call("GetDeadzones", &(id.as_str(),))?;
            Ok((rows, deadzones))
        })();
        match result {
            Ok((rows, (left, right))) => {
                let _ = slint::invoke_from_event_loop(move || {
                    if let Some(ui) = weak.upgrade() {
                        ui.set_controller_left_deadzone(left);
                        ui.set_controller_right_deadzone(right);
                        apply_mapping_rows(&ui, rows);
                    }
                });
            }
            Err(error) => eprintln!("home: controller mapping refresh failed={error}"),
        }
    });
}

fn reset_controller_mapping(ui: &HomeWindow) {
    let id = ui.get_controller_selected_id().to_string();
    ui.set_controller_mapping_reset_armed(false);
    if id.is_empty() {
        return;
    }
    thread::spawn(move || {
        let result = (|| -> zbus::Result<()> {
            let connection = zbus::blocking::Connection::system()?;
            let proxy = controllers_proxy(&connection)?;
            proxy.call("ResetMapping", &(id.as_str(),))
        })();
        /* MappingChanged refreshes the page on success. */
        if let Err(error) = result {
            eprintln!("home: ResetMapping failed={error}");
        }
    });
}

fn set_controller_deadzone(ui: &HomeWindow, stick: &'static str, delta: i32) {
    let id = ui.get_controller_selected_id().to_string();
    if id.is_empty() {
        return;
    }
    let current = if stick == "left" {
        ui.get_controller_left_deadzone()
    } else {
        ui.get_controller_right_deadzone()
    };
    let next = (current + delta).clamp(0, 50);
    if stick == "left" {
        ui.set_controller_left_deadzone(next);
    } else {
        ui.set_controller_right_deadzone(next);
    }

    let weak = ui.as_weak();
    thread::spawn(move || {
        let result = (|| -> zbus::Result<()> {
            let connection = zbus::blocking::Connection::system()?;
            let proxy = controllers_proxy(&connection)?;
            proxy.call("SetDeadzone", &(id.as_str(), stick, next))
        })();
        if let Err(error) = result {
            eprintln!("home: SetDeadzone failed={error}");
        }
        let _ = slint::invoke_from_event_loop(move || {
            if let Some(ui) = weak.upgrade() {
                refresh_controller_mapping(&ui);
            }
        });
    });
}

fn guarded_scroll_offset(index: i32, count: i32, visible: i32, current: i32) -> i32 {
    let visible = visible.max(1);
    if count <= visible || count <= 0 {
        return 0;
    }

    let index = index.clamp(0, count - 1);
    let max_offset = (count - visible).max(0);
    let mut offset = current.clamp(0, max_offset);
    let guard = if visible >= 3 { 1 } else { 0 };
    let top_guard = offset + guard;
    let bottom_guard = offset + visible - 1 - guard;

    if index < top_guard {
        offset = (index - guard).max(0);
    } else if index > bottom_guard {
        offset = index - (visible - 1 - guard);
    }

    offset.clamp(0, max_offset)
}

fn update_controller_list_scroll(ui: &HomeWindow) {
    let count = ui.get_controllers().row_count() as i32;
    ui.set_controller_list_scroll_offset(guarded_scroll_offset(
        ui.get_controller_list_index(),
        count,
        ui.get_settings_list_visible_rows(),
        ui.get_controller_list_scroll_offset(),
    ));
}

fn update_player_assignment_scroll(ui: &HomeWindow) {
    let count = ui.get_player_assignments().row_count() as i32;
    ui.set_player_assignment_scroll_offset(guarded_scroll_offset(
        ui.get_player_assignment_index(),
        count,
        ui.get_settings_list_visible_rows(),
        ui.get_player_assignment_scroll_offset(),
    ));
}

fn update_controller_mapping_scroll(ui: &HomeWindow) {
    let count = ui.get_controller_mapping().row_count() as i32;
    ui.set_controller_mapping_scroll_offset(guarded_scroll_offset(
        ui.get_controller_mapping_index(),
        count,
        ui.get_settings_list_visible_rows(),
        ui.get_controller_mapping_scroll_offset(),
    ));
}

fn open_controller_detail(ui: &HomeWindow, controller: ControllerEntry) {
    ui.set_controller_selected_id(controller.id);
    ui.set_controller_selected_name(controller.name);
    ui.set_controller_selected_transport(controller.transport);
    ui.set_controller_selected_preferred_player(controller.preferred_player);
    ui.set_controller_selected_effective_player(controller.effective_player);
    ui.set_controller_detail_index(0);
    navigate_settings_view(ui, 13);
}

fn update_settings_choice_scroll(ui: &HomeWindow) {
    let count = ui.get_settings_choice_options().row_count() as i32;
    let visible=ui.get_settings_choice_visible_rows().max(1);
    ui.set_settings_choice_scroll(guarded_scroll_offset(
        ui.get_settings_choice_index(),
        count,
        visible,
        ui.get_settings_choice_scroll(),
    ));
}

fn open_settings_choice(
    ui: &HomeWindow,
    context: &str,
    title: &str,
    options: Vec<(String, String)>,
    current_value: &str,
) {
    let entries: Vec<SettingsChoiceEntry> = options
        .into_iter()
        .map(|(value, label)| SettingsChoiceEntry {
            value: value.into(),
            label: label.into(),
        })
        .collect();

    let selected = entries
        .iter()
        .position(|entry| entry.value.as_str() == current_value)
        .unwrap_or(0) as i32;

    ui.set_settings_choice_context(context.into());
    ui.set_settings_choice_title(title.into());
    ui.set_settings_choice_options(ModelRc::from(Rc::new(VecModel::from(entries))));
    ui.set_settings_choice_index(selected);
    ui.set_settings_choice_scroll(0);
    ui.set_settings_choice_open(true);
    update_settings_choice_scroll(ui);
}

fn open_player_assignment_dropdown(ui: &HomeWindow) {
    let player_index = ui.get_player_assignment_index().max(0) as usize;
    let Some(assignment) = ui.get_player_assignments().row_data(player_index) else {
        return;
    };

    let mut options = vec![(String::new(), tr(ui, 122, "Automatic"))];
    for i in 0..ui.get_controllers().row_count() {
        if let Some(controller) = ui.get_controllers().row_data(i) {
            let label = if controller.connected {
                controller.name.to_string()
            } else {
                format!("{} • Unavailable", controller.name)
            };
            options.push((controller.id.to_string(), label));
        }
    }

    open_settings_choice(
        ui,
        "player-assignment",
        &format!("Player {}", assignment.player),
        options,
        assignment.controller_id.as_str(),
    );
}

fn open_lighting_mode_dropdown(ui: &HomeWindow) {
    let values = vec![
        ("off".to_owned(), tr(ui, 54, "Off")),
        ("static".to_owned(), tr(ui, 114, "Static")),
        ("breathe".to_owned(), tr(ui, 115, "Breathe")),
        ("pulse".to_owned(), tr(ui, 117, "Pulse")),
        ("chase".to_owned(), tr(ui, 118, "Chase")),
        ("wave".to_owned(), tr(ui, 119, "Wave")),
        ("rainbow".to_owned(), tr(ui, 116, "Rainbow")),
        ("sparkle".to_owned(), tr(ui, 120, "Sparkle")),
        ("screen".to_owned(), tr(ui, 121, "Screen Reactive")),
    ];
    open_settings_choice(
        ui,
        "lighting-mode",
        "RGB Mode",
        values,
        ui.get_lighting_mode().as_str(),
    );
}

fn open_lighting_color_dropdown(ui: &HomeWindow) {
    let values = [
        ("58,134,255", "Blue"),
        ("171,71,188", "Purple"),
        ("76,175,80", "Green"),
        ("255,152,0", "Orange"),
        ("244,67,54", "Red"),
        ("0,188,212", "Cyan"),
        ("255,255,255", "White"),
        ("255,193,7", "Amber"),
    ];
    let current = format!(
        "{},{},{}",
        ui.get_lighting_red(),
        ui.get_lighting_green(),
        ui.get_lighting_blue()
    );
    open_settings_choice(
        ui,
        "lighting-color",
        "RGB Color",
        values
            .iter()
            .map(|(value, label)| (value.to_string(), label.to_string()))
            .collect(),
        &current,
    );
}

fn audio_socket_command(command: &str) -> std::io::Result<String> {
    let mut stream = UnixStream::connect(AUDIO_SOCKET)?;
    stream.write_all(command.as_bytes())?;
    stream.shutdown(std::net::Shutdown::Write)?;
    let mut reply = String::new();
    BufReader::new(stream).read_to_string(&mut reply)?;
    Ok(reply)
}

fn play_ui_sound(action: &str) {
    let cue = match action {
        "menu_confirm" => Some("select"),
        "menu_back" => Some("back"),
        "menu_up" | "menu_down" | "menu_left" | "menu_right" => Some("navigation"),
        "settings" => Some("quick-settings"),
        _ => None,
    };

    if let Some(cue) = cue {
        let cue = cue.to_owned();
        thread::spawn(move || {
            let _ = audio_socket_command(&format!("SFX PLAY {cue}"));
        });
    }
}

fn adjust_system_sounds_volume(ui: &HomeWindow, delta: i32) {
    let requested = (ui.get_system_sounds_volume() + delta).clamp(0, 100);
    ui.set_system_sounds_volume(requested);
    thread::spawn(move || {
        let _ = audio_socket_command(&format!("SYSTEM VOLUME SET {requested}"));
    });
}

fn adjust_applications_volume(ui: &HomeWindow, delta: i32) {
    let requested = (ui.get_applications_volume() + delta).clamp(0, 100);
    ui.set_applications_volume(requested);
    thread::spawn(move || {
        let _ = audio_socket_command(&format!("APPLICATIONS VOLUME SET {requested}"));
    });
}

fn set_navigation_sounds_enabled(ui: &HomeWindow, enabled: bool) {
    ui.set_navigation_sounds_enabled(enabled);
    thread::spawn(move || {
        let value = if enabled { 1 } else { 0 };
        let _ = audio_socket_command(&format!("SYSTEM NAVIGATION SET {value}"));
    });
}

fn set_power_sounds_enabled(ui: &HomeWindow, enabled: bool) {
    ui.set_power_sounds_enabled(enabled);
    thread::spawn(move || {
        let value = if enabled { 1 } else { 0 };
        let _ = audio_socket_command(&format!("SYSTEM POWER SET {value}"));
    });
}

fn parse_audio_policy_int(command: &str, fallback: i32) -> i32 {
    audio_socket_command(command)
        .ok()
        .and_then(|reply| reply.trim().parse::<i32>().ok())
        .unwrap_or(fallback)
}

fn refresh_audio_policy(ui: &HomeWindow) {
    let weak = ui.as_weak();
    thread::spawn(move || {
        let applications = parse_audio_policy_int("APPLICATIONS VOLUME GET", 100).clamp(0, 100);
        let navigation = parse_audio_policy_int("SYSTEM NAVIGATION GET", 1) != 0;
        let power = parse_audio_policy_int("SYSTEM POWER GET", 1) != 0;
        let _ = slint::invoke_from_event_loop(move || {
            if let Some(ui) = weak.upgrade() {
                ui.set_applications_volume(applications);
                ui.set_navigation_sounds_enabled(navigation);
                ui.set_power_sounds_enabled(power);
            }
        });
    });
}

fn adjust_home_music_volume(ui: &HomeWindow, delta: i32) {
    let requested = (ui.get_home_music_volume() + delta).clamp(0, 100);
    ui.set_home_music_volume(requested);
    thread::spawn(move || {
        let _ = audio_socket_command(&format!("MUSIC VOLUME SET {requested}"));
    });
}

fn refresh_audio(ui: &HomeWindow) {
    let weak = ui.as_weak();
    thread::spawn(move || {
        let result = (|| -> zbus::Result<AudioProductState> {
            let connection = zbus::blocking::Connection::system()?;
            let proxy = audio_proxy(&connection)?;
            let (mode, selected, volume, volume_supported,
                 bluetooth_available, headphones_available,
                 hdmi_available, speaker_available,
                 system_volume, home_music_volume, home_music_playing):
                (String, String, i32, bool, bool, bool, bool, bool, i32, i32, bool) =
                proxy.call("GetSnapshot", &())?;
            Ok(AudioProductState {
                mode,
                selected,
                volume,
                volume_supported,
                bluetooth_available,
                headphones_available,
                hdmi_available,
                speaker_available,
                system_volume,
                home_music_volume,
                home_music_playing,
            })
        })();

        match result {
            Ok(state) => {
                let _ = slint::invoke_from_event_loop(move || {
                    if let Some(ui) = weak.upgrade() {
                        apply_audio_product_state(&ui, state);
                    }
                });
            }
            Err(error) => eprintln!("home: Audio snapshot failed={error}"),
        }
    });
}

fn stop_home_music_session() {
    if let Ok(connection) = zbus::blocking::Connection::system() {
        if let Ok(proxy) = audio_proxy(&connection) {
            let result: zbus::Result<()> = proxy.call("StopHomeMusic", &());
            if let Err(error) = result {
                eprintln!("home: StopHomeMusic failed={error}");
            }
        }
    }
}

fn start_audio_product_listener(ui: &HomeWindow) {
    refresh_audio(ui);
    refresh_audio_policy(ui);
    let weak = ui.as_weak();

    thread::spawn(move || loop {
        let connection = match zbus::blocking::Connection::system() {
            Ok(connection) => connection,
            Err(error) => {
                eprintln!("home: Audio bus failed={error}");
                thread::sleep(Duration::from_millis(500));
                continue;
            }
        };
        let proxy = match audio_proxy(&connection) {
            Ok(proxy) => proxy,
            Err(error) => {
                eprintln!("home: Audio proxy failed={error}");
                thread::sleep(Duration::from_millis(500));
                continue;
            }
        };
        let start_music: zbus::Result<()> = proxy.call("StartHomeMusic", &());
        if let Err(error) = start_music {
            eprintln!("home: StartHomeMusic failed={error}");
        }

        let mut signals = match proxy.receive_signal("StateChanged") {
            Ok(signals) => signals,
            Err(error) => {
                eprintln!("home: Audio signal failed={error}");
                thread::sleep(Duration::from_millis(500));
                continue;
            }
        };

        for message in &mut signals {
            let state = message
                .body()
                .deserialize::<(String, String, i32, bool, bool, bool, bool, bool, i32, i32, bool)>();
            let Ok((mode, selected, volume, volume_supported,
                    bluetooth_available, headphones_available,
                    hdmi_available, speaker_available,
                    system_volume, home_music_volume, home_music_playing)) = state else {
                eprintln!("home: invalid Audio StateChanged payload");
                continue;
            };
            let state = AudioProductState {
                mode,
                selected,
                volume,
                volume_supported,
                bluetooth_available,
                headphones_available,
                hdmi_available,
                speaker_available,
                system_volume,
                home_music_volume,
                home_music_playing,
            };
            let weak = weak.clone();
            let _ = slint::invoke_from_event_loop(move || {
                if let Some(ui) = weak.upgrade() {
                    apply_audio_product_state(&ui, state);
                }
            });
        }

        thread::sleep(Duration::from_millis(250));
    });
}

fn audio_set_output(ui: &HomeWindow, output: String) {
    let weak = ui.as_weak();
    thread::spawn(move || {
        let result = (|| -> zbus::Result<()> {
            let connection = zbus::blocking::Connection::system()?;
            let proxy = audio_proxy(&connection)?;
            proxy.call("SetOutput", &(output.as_str(),))
        })();
        if let Err(error) = result {
            eprintln!("home: Audio SetOutput failed={error}");
        }
        let _ = slint::invoke_from_event_loop(move || {
            if let Some(ui) = weak.upgrade() {
                refresh_audio(&ui);
            }
        });
    });
}

fn audio_adjust_volume(ui: &HomeWindow, delta: i32) {
    if !ui.get_audio_volume_supported() {
        return;
    }

    thread::spawn(move || {
        let result = (|| -> zbus::Result<()> {
            let connection = zbus::blocking::Connection::system()?;
            let proxy = audio_proxy(&connection)?;
            proxy.call("AdjustVolume", &(delta,))
        })();
        if let Err(error) = result {
            eprintln!("home: Audio AdjustVolume failed={error}");
        }
    });
}


fn audio_stop_test_tone(ui: &HomeWindow) {
    ui.set_audio_test_tone_playing(false);
    thread::spawn(move || {
        let _ = Command::new(AUDIO_TEST_TONE)
            .arg("stop")
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .status();
    });
}

fn audio_toggle_test_tone(ui: &HomeWindow) {
    let weak = ui.as_weak();
    thread::spawn(move || {
        let result = Command::new(AUDIO_TEST_TONE)
            .arg("toggle")
            .stdin(Stdio::null())
            .output();

        let playing = match result {
            Ok(output) if output.status.success() => {
                String::from_utf8_lossy(&output.stdout).trim() == "playing"
            }
            Ok(output) => {
                eprintln!(
                    "home: audio test tone failed status={:?}: {}",
                    output.status.code(),
                    String::from_utf8_lossy(&output.stderr).trim()
                );
                false
            }
            Err(error) => {
                eprintln!("home: audio test tone failed={error}");
                false
            }
        };

        let weak_now = weak.clone();
        let _ = slint::invoke_from_event_loop(move || {
            if let Some(ui) = weak_now.upgrade() {
                ui.set_audio_test_tone_playing(playing);
            }
        });

        if !playing {
            return;
        }

        /* The bundled jingle is ten seconds long. Check once after it should
         * have completed; no persistent polling is introduced. */
        thread::sleep(Duration::from_millis(10_250));
        let still_playing = Command::new(AUDIO_TEST_TONE)
            .arg("status")
            .stdin(Stdio::null())
            .output()
            .ok()
            .map(|output| {
                output.status.success()
                    && String::from_utf8_lossy(&output.stdout).trim() == "playing"
            })
            .unwrap_or(false);

        let _ = slint::invoke_from_event_loop(move || {
            if let Some(ui) = weak.upgrade() {
                ui.set_audio_test_tone_playing(still_playing);
            }
        });
    });
}

fn open_audio_output_dropdown(ui: &HomeWindow) {
    let mut options = vec![
        ("auto".to_owned(), tr(ui, 122, "Automatic")),
    ];

    if ui.get_audio_bluetooth_available() {
        options.push(("bluetooth".to_owned(), tr(ui, 331, "Bluetooth Audio")));
    }

    /* Speaker/headphones are one physical analog route.  Jack insertion
     * selects the physical destination; the product UI must not imply it can
     * override that hardware routing. */
    options.push(("analog".to_owned(), tr(ui, 332, "Analog Audio")));

    if ui.get_audio_hdmi_available() {
        options.push(("hdmi".to_owned(), "HDMI".to_owned()));
    }

    open_settings_choice(
        ui,
        "audio-output",
        &tr(ui, 133, "Audio Output"),
        options,
        ui.get_audio_mode().as_str(),
    );
}


fn open_system_profile_dropdown(ui: &HomeWindow) {
    open_settings_choice(
        ui,
        "system-profile",
        &tr(ui, 50, "Performance Profile"),
        vec![
            ("auto".to_owned(), tr(ui, 52, "Auto")),
            ("battery-saver".to_owned(), tr(ui, 51, "Battery Saver")),
        ],
        ui.get_system_profile().as_str(),
    );
}

fn open_storage_backup_policy_dropdown(ui: &HomeWindow) {
    open_settings_choice(
        ui,
        "system-storage-backup-policy",
        &tr(ui, 69, "Backup Policy"),
        vec![
            ("OFF".to_owned(), tr(ui, 54, "Off")),
            ("DAILY".to_owned(), tr(ui, 70, "Daily")),
            ("WEEKLY".to_owned(), tr(ui, 71, "Weekly")),
            ("MONTHLY".to_owned(), tr(ui, 72, "Monthly")),
        ],
        ui.get_system_backup_policy().as_str(),
    );
}

fn open_auto_battery_dropdown(ui: &HomeWindow) {
    let values = [0, 10, 15, 20, 25, 30, 40, 50];
    let options = values
        .into_iter()
        .map(|value| {
            (
                value.to_string(),
                if value == 0 { tr(ui, 54, "Off") } else { format!("≤ {}%", value) },
            )
        })
        .collect();
    open_settings_choice(
        ui,
        "system-auto-battery",
        &tr(ui, 53, "Automatic Battery Saver"),
        options,
        ui.get_system_auto_battery_threshold().to_string().as_str(),
    );
}

fn open_screensaver_timeout_dropdown(ui: &HomeWindow) {
    let values = [0, 1, 2, 5, 10, 15, 30, 45, 60];
    let options = values
        .into_iter()
        .map(|value| {
            (
                value.to_string(),
                if value == 0 { tr(ui, 54, "Off") } else { format!("{} min", value) },
            )
        })
        .collect();
    open_settings_choice(
        ui,
        "system-screensaver",
        &tr(ui, 55, "Screensaver After"),
        options,
        ui.get_system_screensaver_after_min().to_string().as_str(),
    );
}

fn open_sleep_timeout_dropdown(ui: &HomeWindow) {
    let values = [0, 1, 2, 5, 10, 15, 30, 45, 60, 120];
    let options = values
        .into_iter()
        .map(|value| {
            (
                value.to_string(),
                if value == 0 { tr(ui, 54, "Off") } else { format!("{} min", value) },
            )
        })
        .collect();
    open_settings_choice(
        ui,
        "system-sleep",
        &tr(ui, 56, "Sleep After"),
        options,
        ui.get_system_sleep_after_min().to_string().as_str(),
    );
}

fn open_poweroff_timeout_dropdown(ui: &HomeWindow) {
    let values = [0, 5, 10, 15, 30, 45, 60, 90, 120, 180, 240];
    let options = values
        .into_iter()
        .map(|value| {
            (
                value.to_string(),
                if value == 0 { tr(ui, 54, "Off") } else { format!("{} min", value) },
            )
        })
        .collect();
    open_settings_choice(
        ui,
        "system-poweroff",
        &tr(ui, 58, "Power Off"),
        options,
        ui.get_system_poweroff_after_min().to_string().as_str(),
    );
}

fn refresh_display_state(ui: &HomeWindow) {
    let weak = ui.as_weak();
    thread::spawn(move || {
        let brightness = Command::new(DISPLAYCTL)
            .arg("brightness")
            .stdin(Stdio::null())
            .output()
            .ok()
            .and_then(|output| {
                String::from_utf8_lossy(&output.stdout)
                    .trim()
                    .parse::<i32>()
                    .ok()
            })
            .unwrap_or(60)
            .clamp(1, 100);

        let hdmi_connected = Command::new(DISPLAYCTL)
            .arg("hdmi")
            .stdin(Stdio::null())
            .output()
            .ok()
            .map(|output| String::from_utf8_lossy(&output.stdout).trim() == "connected")
            .unwrap_or(false);

        let temp_status = Command::new(DISPLAYCTL)
            .arg("temperature-supported")
            .stdin(Stdio::null())
            .output();
        let temp_available = temp_status
            .as_ref()
            .map(|o| o.status.success())
            .unwrap_or(false);
        let temperature = Command::new(DISPLAYCTL)
            .arg("temperature")
            .stdin(Stdio::null())
            .output()
            .ok()
            .and_then(|o| String::from_utf8_lossy(&o.stdout).trim().parse::<i32>().ok())
            .unwrap_or(6500);

        let _ = slint::invoke_from_event_loop(move || {
            if let Some(ui) = weak.upgrade() {
                ui.set_display_brightness(brightness);
                ui.set_display_brightness_available(!hdmi_connected);
                ui.set_display_color_temperature_available(temp_available);
                ui.set_display_color_temperature(temperature);
                /* Color Temperature is the last Display & Audio row (9). */
                if !temp_available && ui.get_display_audio_index() == 9 {
                    ui.set_display_audio_index(8);
                }
            }
        });
    });
}

fn adjust_display_brightness(ui: &HomeWindow, delta: i32) {
    if !ui.get_display_brightness_available() {
        return;
    }

    let requested = (ui.get_display_brightness() + delta).clamp(1, 100);
    ui.set_display_brightness(requested);
    let weak = ui.as_weak();

    thread::spawn(move || {
        let result = Command::new(DISPLAYCTL)
            .arg("brightness")
            .arg(requested.to_string())
            .stdin(Stdio::null())
            .output();

        if let Err(error) = result {
            eprintln!("home: brightness update failed={error}");
        }

        let _ = slint::invoke_from_event_loop(move || {
            if let Some(ui) = weak.upgrade() {
                refresh_display_state(&ui);
            }
        });
    });
}

fn adjust_display_color_temperature(ui: &HomeWindow, delta: i32) {
    if !ui.get_display_color_temperature_available() {
        return;
    }
    let requested = (ui.get_display_color_temperature() + delta).clamp(3500, 7500);
    ui.set_display_color_temperature(requested);
    let weak = ui.as_weak();
    thread::spawn(move || {
        let result = Command::new(DISPLAYCTL)
            .arg("temperature")
            .arg(requested.to_string())
            .stdin(Stdio::null())
            .output();
        if let Err(error) = result {
            eprintln!("home: color temperature update failed={error}");
        }
        let _ = slint::invoke_from_event_loop(move || {
            if let Some(ui) = weak.upgrade() {
                refresh_display_state(&ui);
            }
        });
    });
}

fn apply_settings_choice(ui: &HomeWindow) {
    let index = ui.get_settings_choice_index().max(0) as usize;
    let Some(entry) = ui.get_settings_choice_options().row_data(index) else {
        return;
    };
    let context = ui.get_settings_choice_context().to_string();
    let value = entry.value.to_string();
    if context.as_str() != "timezone-region" {
        ui.set_settings_choice_open(false);
    }

    match context.as_str() {
        "player-assignment" => {
            let player_index = ui.get_player_assignment_index().max(0) as usize;
            let Some(assignment) = ui.get_player_assignments().row_data(player_index) else {
                return;
            };
            let player = assignment.player;
            let name = if value.is_empty() {
                String::new()
            } else {
                let mut found = String::new();
                for i in 0..ui.get_controllers().row_count() {
                    if let Some(controller) = ui.get_controllers().row_data(i) {
                        if controller.id.as_str() == value {
                            found = controller.name.to_string();
                            break;
                        }
                    }
                }
                found
            };
            let weak = ui.as_weak();
            thread::spawn(move || {
                let result = (|| -> zbus::Result<()> {
                    let connection = zbus::blocking::Connection::system()?;
                    let proxy = controllers_proxy(&connection)?;
                    proxy.call("SetPlayerPreference", &(player, value.as_str(), name.as_str()))
                })();
                if let Err(error) = result {
                    eprintln!("home: player assignment failed={error}");
                }
                let _ = slint::invoke_from_event_loop(move || {
                    if let Some(ui) = weak.upgrade() {
                        refresh_controllers(&ui);
                    }
                });
            });
        }
        "lighting-mode" => {
            lighting_call(ui, "SetMode", LightingArgs::Mode(value));
        }
        "lighting-color" => {
            let parts: Vec<i32> = value
                .split(',')
                .filter_map(|part| part.parse::<i32>().ok())
                .collect();
            if parts.len() == 3 {
                lighting_call(ui, "SetColor", LightingArgs::Color(parts[0], parts[1], parts[2]));
            }
        }
        "audio-output" => {
            audio_set_output(ui, value);
        }
        "system-profile" => {
            system_set(ui, format!("SET PROFILE {}", value));
        }
        "system-auto-battery" => {
            system_set(ui, format!("SET AUTO_BATTERY {}", value));
        }
        "system-screensaver" => {
            system_set(ui, format!("SET SCREENSAVER {}", value));
        }
        "system-sleep" => {
            system_set(ui, format!("SET SLEEP {}", value));
        }
        "system-poweroff" => {
            system_set(ui, format!("SET POWEROFF {}", value));
        }
        "system-storage-backup-policy" => {
            system_set(ui, format!("SET STORAGE BACKUP_POLICY {}", value));
        }
        "ui-language" => { let v=value.clone();thread::spawn(move||{let _=localization_command(&format!("SET LANGUAGE {}",v));}); }
        "user-login-mode" => {thread::spawn(move||{let _=users_command(&format!("SET_LOGIN_MODE	{}",value));});}
        "default-user" => {thread::spawn(move||{let _=users_command(&format!("SET_DEFAULT	{}",value));});}
        "timezone-region" => { open_timezone_city(ui,value); }
        "timezone-city" => {thread::spawn(move||{let _=regional_command(&format!("SET_TIMEZONE	{}",value));});}
        "keyboard-layout" => {thread::spawn(move||{let _=regional_command(&format!("SET_KEYBOARD	{}",value));});}
        _ => {}
    }
}

fn set_controller_tester(ui: &HomeWindow, enabled: bool) {
    let id = ui.get_controller_selected_id().to_string();
    if enabled && id.is_empty() {
        return;
    }

    if enabled {
        ui.set_tester_action(tr(ui, 249, "Press any control").into());
        reset_tester_visual_state(ui);
    }

    thread::spawn(move || {
        if let Ok(connection) = zbus::blocking::Connection::system() {
            if let Ok(proxy) = controllers_proxy(&connection) {
                let _: zbus::Result<()> =
                    proxy.call("SetTester", &(enabled, if enabled { id.as_str() } else { "" }));
            }
        }
    });
}

fn begin_controller_remap(ui: &HomeWindow) {
    let id = ui.get_controller_selected_id().to_string();
    let Some(entry) = selected_mapping_entry(ui) else {
        return;
    };
    if entry.kind != 0 && entry.kind != 1 {
        return;
    }
    let control = entry.control.to_string();
    let prompt = if entry.kind == 0 {
        tr(ui, 362, "Press the new control • wait 5 s to cancel")
    } else if control.ends_with("_x") {
        tr(ui, 363, "Move the stick right • any button cancels")
    } else {
        tr(ui, 364, "Move the stick down • any button cancels")
    };

    ui.set_controller_remap_waiting(true);
    ui.set_controller_remap_prompt(prompt.into());

    let weak = ui.as_weak();
    thread::spawn(move || {
        let result = (|| -> zbus::Result<()> {
            let connection = zbus::blocking::Connection::system()?;
            let proxy = controllers_proxy(&connection)?;
            proxy.call("BeginRemap", &(id.as_str(), control.as_str()))
        })();
        if let Err(error) = result {
            let _ = slint::invoke_from_event_loop(move || {
                if let Some(ui) = weak.upgrade() {
                    ui.set_controller_remap_waiting(false);
                    eprintln!("home: BeginRemap failed={error}");
                }
            });
        }
    });
}

fn cancel_controller_remap(ui: &HomeWindow) {
    if !ui.get_controller_remap_waiting() {
        return;
    }
    ui.set_controller_remap_waiting(false);
    thread::spawn(move || {
        if let Ok(connection) = zbus::blocking::Connection::system() {
            if let Ok(proxy) = controllers_proxy(&connection) {
                let _: zbus::Result<()> = proxy.call("CancelRemap", &());
            }
        }
    });
}

fn refresh_rumble(ui: &HomeWindow) {
    let weak = ui.as_weak();
    thread::spawn(move || {
        let result = (|| -> zbus::Result<(bool, bool)> {
            let connection = zbus::blocking::Connection::system()?;
            let proxy = rumble_proxy(&connection)?;
            proxy.call("GetState", &())
        })();
        if let Ok((supported, enabled)) = result {
            let _ = slint::invoke_from_event_loop(move || {
                if let Some(ui) = weak.upgrade() {
                    ui.set_rumble_supported(supported);
                    ui.set_rumble_enabled(enabled);
                }
            });
        }
    });
}

fn rumble_set_enabled(ui: &HomeWindow, enabled: bool) {
    let weak = ui.as_weak();
    thread::spawn(move || {
        let result = (|| -> zbus::Result<()> {
            let connection = zbus::blocking::Connection::system()?;
            let proxy = rumble_proxy(&connection)?;
            proxy.call("SetEnabled", &(enabled,))
        })();
        if result.is_ok() {
            let _ = slint::invoke_from_event_loop(move || {
                if let Some(ui) = weak.upgrade() {
                    ui.set_rumble_enabled(enabled);
                }
            });
        } else if let Err(error) = result {
            eprintln!("home: rumble toggle failed={error}");
        }
    });
}

fn rumble_test() {
    thread::spawn(move || {
        if let Ok(connection) = zbus::blocking::Connection::system() {
            if let Ok(proxy) = rumble_proxy(&connection) {
                let result: zbus::Result<()> = proxy.call("Test", &());
                if let Err(error) = result {
                    eprintln!("home: rumble test failed={error}");
                }
            }
        }
    });
}

fn apply_lighting_state(
    ui: &HomeWindow,
    state: (bool, String, i32, i32, i32, i32, bool, String),
) {
    let (supported, mode, brightness, red, green, blue, system_effects, active_effect) = state;
    ui.set_lighting_supported(supported);
    ui.set_lighting_mode(mode.into());
    ui.set_lighting_brightness(brightness);
    ui.set_lighting_red(red);
    ui.set_lighting_green(green);
    ui.set_lighting_blue(blue);
    ui.set_lighting_color_name(lighting_color_name(red, green, blue).into());
    ui.set_lighting_system_effects(system_effects);
    ui.set_lighting_active_effect(active_effect.into());
}

fn refresh_lighting(ui: &HomeWindow) {
    let weak = ui.as_weak();
    thread::spawn(move || {
        let result = (|| -> zbus::Result<(bool, String, i32, i32, i32, i32, bool, String)> {
            let connection = zbus::blocking::Connection::system()?;
            let proxy = lighting_proxy(&connection)?;
            proxy.call("GetState", &())
        })();
        if let Ok(state) = result {
            let _ = slint::invoke_from_event_loop(move || {
                if let Some(ui) = weak.upgrade() {
                    apply_lighting_state(&ui, state);
                }
            });
        }
    });
}

fn lighting_call(ui: &HomeWindow, method: &'static str, args: LightingArgs) {
    let weak = ui.as_weak();
    thread::spawn(move || {
        let result = (|| -> zbus::Result<()> {
            let connection = zbus::blocking::Connection::system()?;
            let proxy = lighting_proxy(&connection)?;
            match args {
                LightingArgs::Mode(value) => proxy.call(method, &(value.as_str(),)),
                LightingArgs::Brightness(value) => proxy.call(method, &(value,)),
                LightingArgs::Color(r, g, b) => proxy.call(method, &(r, g, b)),
                LightingArgs::Toggle(value) => proxy.call(method, &(value,)),
                LightingArgs::Effect(effect, context) => {
                    proxy.call(method, &(effect.as_str(), context.as_str()))
                }
            }
        })();
        if let Err(error) = result {
            eprintln!("home: lighting {method} failed={error}");
        }
        let _ = slint::invoke_from_event_loop(move || {
            if let Some(ui) = weak.upgrade() {
                refresh_lighting(&ui);
            }
        });
    });
}

enum LightingArgs {
    Mode(String),
    Brightness(i32),
    Color(i32, i32, i32),
    Toggle(bool),
    Effect(String, String),
}

fn lighting_stop_effect(ui: &HomeWindow) {
    let weak = ui.as_weak();
    thread::spawn(move || {
        if let Ok(connection) = zbus::blocking::Connection::system() {
            if let Ok(proxy) = lighting_proxy(&connection) {
                let _: zbus::Result<()> = proxy.call("StopEffect", &());
            }
        }
        let _ = slint::invoke_from_event_loop(move || {
            if let Some(ui) = weak.upgrade() {
                refresh_lighting(&ui);
            }
        });
    });
}

fn preview_lighting_effect(ui: &HomeWindow) {
    let index = ui.get_lighting_preview_index().rem_euclid(5);
    let (effect, context) = match index {
        0 => ("low-battery", ""),
        1 => ("controller-connected", ""),
        2 => ("wifi-search", ""),
        3 => ("boot", ""),
        _ => ("system", "preview-system"),
    };
    lighting_call(
        ui,
        "PlayEffect",
        LightingArgs::Effect(effect.to_owned(), context.to_owned()),
    );
}

fn start_controller_signal_listener(ui: &HomeWindow, signal_name: &'static str) {
    let weak = ui.as_weak();
    thread::spawn(move || {
        let connection = match zbus::blocking::Connection::system() {
            Ok(connection) => connection,
            Err(error) => {
                eprintln!("home: Controllers bus failed={error}");
                return;
            }
        };
        let proxy = match controllers_proxy(&connection) {
            Ok(proxy) => proxy,
            Err(error) => {
                eprintln!("home: Controllers proxy failed={error}");
                return;
            }
        };
        let mut signals = match proxy.receive_signal(signal_name) {
            Ok(signals) => signals,
            Err(error) => {
                eprintln!("home: Controllers signal {signal_name} failed={error}");
                return;
            }
        };

        for message in &mut signals {
            match signal_name {
                "DevicesChanged" => {
                    let body = message
                        .body()
                        .deserialize::<(Vec<(String, String, String, bool, bool, i32, i32, i32)>,)>();
                    if let Ok((rows,)) = body {
                        let weak = weak.clone();
                        let _ = slint::invoke_from_event_loop(move || {
                            if let Some(ui) = weak.upgrade() {
                                apply_controller_rows(&ui, rows);
                            }
                        });
                    }
                }
                "AssignmentsChanged" => {
                    let body = message
                        .body()
                        .deserialize::<(Vec<(i32, String, String, bool)>,)>();
                    if let Ok((rows,)) = body {
                        let weak = weak.clone();
                        let _ = slint::invoke_from_event_loop(move || {
                            if let Some(ui) = weak.upgrade() {
                                apply_player_assignment_rows(&ui, rows);
                            }
                        });
                    }
                }
                "MappingChanged" => {
                    let body = message.body().deserialize::<(String,)>();
                    if let Ok((id,)) = body {
                        let weak = weak.clone();
                        let _ = slint::invoke_from_event_loop(move || {
                            if let Some(ui) = weak.upgrade() {
                                if ui.get_controller_selected_id().as_str() == id {
                                    refresh_controller_mapping(&ui);
                                }
                            }
                        });
                    }
                }
                "InputEvent" => {
                    let body = message
                        .body()
                        .deserialize::<(String, String, i32, i32, i32)>();
                    if let Ok((id, control, event_type, code, value)) = body {
                        let weak = weak.clone();
                        let _ = slint::invoke_from_event_loop(move || {
                            if let Some(ui) = weak.upgrade() {
                                if ui.get_controller_selected_id().as_str() == id {
                                    set_tester_visual_state(&ui, &control, value);
                                    if event_type != 1 || value != 0 {
                                        let label = tester_display_label(
                                            &ui,
                                            &control,
                                            event_type,
                                            code,
                                            value,
                                        );
                                        ui.set_tester_action(label.into());
                                    }
                                }
                            }
                        });
                    }
                }
                "TesterExitRequested" => {
                    let body = message.body().deserialize::<(String,)>();
                    if let Ok((id,)) = body {
                        let weak = weak.clone();
                        let _ = slint::invoke_from_event_loop(move || {
                            if let Some(ui) = weak.upgrade() {
                                if ui.get_controller_selected_id().as_str() == id
                                    && ui.get_settings_view() == 14
                                {
                                    navigate_settings_view(
                                        &ui,
                                        ui.get_controller_tester_return_view(),
                                    );
                                }
                            }
                        });
                    }
                }
                "RemapCaptured" | "RemapCancelled" => {
                    /* RemapCaptured carries (id, control, source), RemapCancelled
                     * (id, control); both end the capture. MappingChanged
                     * refreshes the rows after a capture. */
                    let id = message
                        .body()
                        .deserialize::<(String, String, String)>()
                        .map(|(id, _, _)| id)
                        .or_else(|_| {
                            message.body().deserialize::<(String, String)>().map(|(id, _)| id)
                        });
                    if let Ok(id) = id {
                        let weak = weak.clone();
                        let _ = slint::invoke_from_event_loop(move || {
                            if let Some(ui) = weak.upgrade() {
                                if ui.get_controller_selected_id().as_str() == id {
                                    ui.set_controller_remap_waiting(false);
                                }
                            }
                        });
                    }
                }
                _ => {}
            }
        }
    });
}

fn start_controllers_product_listener(ui: &HomeWindow) {
    refresh_controllers(ui);
    start_controller_signal_listener(ui, "DevicesChanged");
    start_controller_signal_listener(ui, "AssignmentsChanged");
    start_controller_signal_listener(ui, "MappingChanged");
    start_controller_signal_listener(ui, "InputEvent");
    start_controller_signal_listener(ui, "RemapCaptured");
    start_controller_signal_listener(ui, "RemapCancelled");
    start_controller_signal_listener(ui, "TesterExitRequested");
}

fn start_rumble_listener(ui: &HomeWindow) {
    refresh_rumble(ui);
    let weak = ui.as_weak();
    thread::spawn(move || {
        let Ok(connection) = zbus::blocking::Connection::system() else {
            return;
        };
        let Ok(proxy) = rumble_proxy(&connection) else {
            return;
        };
        let Ok(mut signals) = proxy.receive_signal("StateChanged") else {
            return;
        };

        for message in &mut signals {
            if let Ok((supported, enabled)) = message.body().deserialize::<(bool, bool)>() {
                let weak = weak.clone();
                let _ = slint::invoke_from_event_loop(move || {
                    if let Some(ui) = weak.upgrade() {
                        ui.set_rumble_supported(supported);
                        ui.set_rumble_enabled(enabled);
                    }
                });
            }
        }
    });
}

fn start_lighting_listener(ui: &HomeWindow) {
    refresh_lighting(ui);
    let weak = ui.as_weak();
    thread::spawn(move || {
        let Ok(connection) = zbus::blocking::Connection::system() else {
            return;
        };
        let Ok(proxy) = lighting_proxy(&connection) else {
            return;
        };
        let Ok(mut signals) = proxy.receive_signal("StateChanged") else {
            return;
        };

        for message in &mut signals {
            if let Ok(state) = message
                .body()
                .deserialize::<(bool, String, i32, i32, i32, i32, bool, String)>()
            {
                let weak = weak.clone();
                let _ = slint::invoke_from_event_loop(move || {
                    if let Some(ui) = weak.upgrade() {
                        apply_lighting_state(&ui, state);
                    }
                });
            }
        }
    });
}

fn update_bluetooth_scroll(ui: &HomeWindow, known: bool) {
    let index = if known { ui.get_bluetooth_known_index() } else { ui.get_bluetooth_device_index() };
    let count = if known {
        ui.get_bluetooth_known_devices().row_count() as i32
    } else {
        ui.get_bluetooth_devices().row_count() as i32
    };
    let current = if known {
        ui.get_bluetooth_known_scroll_offset()
    } else {
        ui.get_bluetooth_device_scroll_offset()
    };
    let offset = guarded_scroll_offset(index, count, ui.get_settings_list_visible_rows(), current);

    if known {
        ui.set_bluetooth_known_scroll_offset(offset);
    } else {
        ui.set_bluetooth_device_scroll_offset(offset);
    }
}

fn open_bluetooth_detail(ui: &HomeWindow, device: BluetoothDeviceEntry, return_view: i32) {
    ui.set_bluetooth_selected_address(device.address);
    ui.set_bluetooth_selected_name(device.name);
    ui.set_bluetooth_selected_kind(device.kind);
    ui.set_bluetooth_selected_paired(device.paired);
    ui.set_bluetooth_selected_connected(device.connected);
    ui.set_bluetooth_selected_trusted(device.trusted);
    ui.set_bluetooth_detail_index(0);
    ui.set_bluetooth_detail_return_view(return_view);
    bluetooth_notice(ui, "");
    navigate_settings_view(ui, 9);
}

fn wifi_notice(ui: &HomeWindow, text: impl Into<SharedString>) {
    ui.set_wifi_notice(text.into());
}

fn wifi_set_enabled(ui: &HomeWindow, enabled: bool) {
    let weak = ui.as_weak();
    wifi_notice(ui, if enabled { tr(ui, 250, "Enabling Wi-Fi…") } else { tr(ui, 251, "Disabling Wi-Fi…") });
    thread::spawn(move || {
        let result = (|| -> zbus::Result<()> {
            let connection = zbus::blocking::Connection::system()?;
            let proxy = wifi_proxy(&connection)?;
            proxy.call("SetEnabled", &(enabled,))
        })();
        let _ = slint::invoke_from_event_loop(move || {
            if let Some(ui) = weak.upgrade() {
                match result {
                    Ok(()) => wifi_notice(&ui, ""),
                    Err(error) => wifi_notice(&ui, tr_arg(&ui, 252, "Wi-Fi toggle failed: {0}", &error.to_string())),
                }
            }
        });
    });
}

fn wifi_start_scan_session(ui: &HomeWindow) {
    let weak = ui.as_weak();
    ui.set_wifi_scanning(true);
    thread::spawn(move || {
        let result = (|| -> zbus::Result<()> {
            let connection = zbus::blocking::Connection::system()?;
            let proxy = wifi_proxy(&connection)?;
            proxy.call("StartScanSession", &())
        })();
        if let Err(error) = result {
            let _ = slint::invoke_from_event_loop(move || {
                if let Some(ui) = weak.upgrade() {
                    ui.set_wifi_scanning(false);
                    wifi_notice(&ui, tr_arg(&ui, 253, "Scan failed: {0}", &error.to_string()));
                }
            });
        }
    });
}

fn wifi_stop_scan_session(ui: &HomeWindow) {
    ui.set_wifi_scanning(false);
    thread::spawn(move || {
        let _ = (|| -> zbus::Result<()> {
            let connection = zbus::blocking::Connection::system()?;
            let proxy = wifi_proxy(&connection)?;
            proxy.call("StopScanSession", &())
        })();
    });
}

fn navigate_settings_view(ui: &HomeWindow, next_view: i32) {
    let previous_view = ui.get_settings_view();

    if previous_view == 2 && next_view != 2 {
        wifi_stop_scan_session(ui);
    }
    if previous_view == 7 && next_view != 7 {
        bluetooth_stop_discovery_session(ui);
    }
    if previous_view == 14 && next_view != 14 {
        set_controller_tester(ui, false);
    }
    if previous_view == 15 && next_view != 15 {
        cancel_controller_remap(ui);
        ui.set_controller_mapping_reset_armed(false);
    }
    if previous_view == 17 && next_view != 17 && ui.get_lighting_preview_open() {
        ui.set_lighting_preview_open(false);
        lighting_stop_effect(ui);
    }
    if previous_view == 23 && next_view == 0 {
        /* Returning to the rail previews General from the top again. */
        ui.set_general_scroll_offset(0);
    }
    if previous_view == 18 && next_view != 18 {
        /* settings-view 0 is only the category-rail focus state. The right pane
         * is the same real page. Reset its viewport when focus returns to the
         * rail so re-entering starts from the top without changing content. */
        ui.set_display_audio_scroll_offset(0);
    }
    if ui.get_settings_choice_open() {
        ui.set_settings_choice_open(false);
    }

    ui.set_settings_view(next_view);
    set_system_info_live(next_view == 21);

    if previous_view != 2 && next_view == 2 {
        ui.set_wifi_network_index(0);
        ui.set_wifi_network_scroll_offset(0);
        wifi_notice(ui, "");
        refresh_wifi_networks(ui, false);
        wifi_start_scan_session(ui);
    }

    if previous_view != 7 && next_view == 7 {
        ui.set_bluetooth_device_index(0);
        ui.set_bluetooth_device_scroll_offset(0);
        bluetooth_notice(ui, "");
        refresh_bluetooth_devices(ui);
        bluetooth_start_discovery_session(ui);
    }
    if next_view == 10 {
        refresh_controllers(ui);
        refresh_rumble(ui);
        refresh_lighting(ui);
    }
    if next_view == 11 || next_view == 12 {
        refresh_controllers(ui);
    }
    if previous_view != 14 && next_view == 14 {
        set_controller_tester(ui, true);
    }
    if next_view == 15 {
        refresh_controller_mapping(ui);
    }
    if next_view == 16 {
        refresh_rumble(ui);
    }
    if next_view == 17 {
        refresh_lighting(ui);
    }
    if next_view == 18 {
        refresh_display_state(ui);
        refresh_audio(ui);
        refresh_audio_policy(ui);
    }
    if matches!(next_view, 19 | 20 | 21 | 22) {
        refresh_system(ui);
        if next_view != 22 {
            ui.set_system_reset_confirm(false);
        }
    }
}

fn wifi_connect(ui: &HomeWindow, ssid: String, password: String, hidden: bool) {
    let weak = ui.as_weak();
    wifi_notice(ui, tr_arg(ui, 254, "Connecting to {0}…", &ssid));
    thread::spawn(move || {
        let result = (|| -> zbus::Result<()> {
            let connection = zbus::blocking::Connection::system()?;
            let proxy = wifi_proxy(&connection)?;
            proxy.call("Connect", &(ssid.as_str(), password.as_str(), hidden))
        })();
        let _ = slint::invoke_from_event_loop(move || {
            if let Some(ui) = weak.upgrade() {
                match result {
                    Ok(()) => {
                        wifi_notice(&ui, "");
                        refresh_wifi_networks(&ui, false);
                        refresh_wifi_networks(&ui, true);
                    }
                    Err(error) => wifi_notice(&ui, tr_arg(&ui, 255, "Connect failed: {0}", &error.to_string())),
                }
            }
        });
    });
}

fn wifi_disconnect(ui: &HomeWindow) {
    let weak = ui.as_weak();
    wifi_notice(ui, tr(ui, 256, "Disconnecting…"));
    thread::spawn(move || {
        let result = (|| -> zbus::Result<()> {
            let connection = zbus::blocking::Connection::system()?;
            let proxy = wifi_proxy(&connection)?;
            proxy.call("Disconnect", &())
        })();
        let _ = slint::invoke_from_event_loop(move || {
            if let Some(ui) = weak.upgrade() {
                match result {
                    Ok(()) => {
                        wifi_notice(&ui, "");
                        ui.set_wifi_selected_current(false);
                        refresh_wifi_networks(&ui, false);
                        refresh_wifi_networks(&ui, true);
                    }
                    Err(error) => wifi_notice(&ui, tr_arg(&ui, 257, "Disconnect failed: {0}", &error.to_string())),
                }
            }
        });
    });
}

fn wifi_forget(ui: &HomeWindow, ssid: String) {
    let weak = ui.as_weak();
    wifi_notice(ui, tr_arg(ui, 258, "Forgetting {0}…", &ssid));
    thread::spawn(move || {
        let result = (|| -> zbus::Result<()> {
            let connection = zbus::blocking::Connection::system()?;
            let proxy = wifi_proxy(&connection)?;
            proxy.call("Forget", &(ssid.as_str(),))
        })();
        let _ = slint::invoke_from_event_loop(move || {
            if let Some(ui) = weak.upgrade() {
                match result {
                    Ok(()) => {
                        wifi_notice(&ui, "");
                        navigate_settings_view(&ui, 4);
                        refresh_wifi_networks(&ui, true);
                        refresh_wifi_networks(&ui, false);
                    }
                    Err(error) => wifi_notice(&ui, tr_arg(&ui, 259, "Forget failed: {0}", &error.to_string())),
                }
            }
        });
    });
}

fn fetch_wifi_ip_configuration(ui: &HomeWindow, ssid: String) {
    let weak = ui.as_weak();
    thread::spawn(move || {
        let result = (|| -> zbus::Result<(String, String, u32, String, String, String, String, u32, String, String, String)> {
            let connection = zbus::blocking::Connection::system()?;
            let proxy = wifi_proxy(&connection)?;
            proxy.call("GetIpConfiguration", &(ssid.as_str(),))
        })();
        let _ = slint::invoke_from_event_loop(move || {
            if let Some(ui) = weak.upgrade() {
                match result {
                    Ok((mode, address, prefix, netmask, gateway, dns,
                        runtime_address, runtime_prefix, runtime_netmask,
                        runtime_gateway, runtime_dns)) => {
                        ui.set_wifi_ip_mode(mode.into());
                        ui.set_wifi_ip_address(address.into());
                        ui.set_wifi_ip_prefix(prefix as i32);
                        ui.set_wifi_ip_netmask(netmask.into());
                        ui.set_wifi_ip_gateway(gateway.into());
                        ui.set_wifi_ip_dns(dns.into());
                        ui.set_wifi_runtime_address(runtime_address.into());
                        ui.set_wifi_runtime_prefix(runtime_prefix as i32);
                        ui.set_wifi_runtime_netmask(runtime_netmask.into());
                        ui.set_wifi_runtime_gateway(runtime_gateway.into());
                        ui.set_wifi_runtime_dns(runtime_dns.into());
                        ui.set_wifi_ip_index(0);
                        wifi_notice(&ui, "");
                    }
                    Err(error) => wifi_notice(&ui, tr_arg(&ui, 260, "IP configuration failed: {0}", &error.to_string())),
                }
            }
        });
    });
}

fn save_wifi_ip_configuration(ui: &HomeWindow) {
    let ssid = ui.get_wifi_selected_ssid().to_string();
    let mode = ui.get_wifi_ip_mode().to_string();
    let address = ui.get_wifi_ip_address().to_string();
    let prefix = ui.get_wifi_ip_prefix().clamp(0, 32) as u32;
    let gateway = ui.get_wifi_ip_gateway().to_string();
    let dns = ui.get_wifi_ip_dns().to_string();
    let weak = ui.as_weak();
    wifi_notice(ui, tr(ui, 261, "Applying network settings…"));
    thread::spawn(move || {
        let result = (|| -> zbus::Result<()> {
            let connection = zbus::blocking::Connection::system()?;
            let proxy = wifi_proxy(&connection)?;
            proxy.call(
                "SetIpConfiguration",
                &(ssid.as_str(), mode.as_str(), address.as_str(), prefix,
                  gateway.as_str(), dns.as_str()),
            )
        })();
        let _ = slint::invoke_from_event_loop(move || {
            if let Some(ui) = weak.upgrade() {
                match result {
                    Ok(()) => wifi_notice(&ui, ""),
                    Err(error) => wifi_notice(&ui, tr_arg(&ui, 260, "IP configuration failed: {0}", &error.to_string())),
                }
            }
        });
    });
}

fn prefix_to_netmask(prefix: i32) -> String {
    if !(0..=32).contains(&prefix) {
        return String::new();
    }
    let mask = if prefix == 0 { 0u32 } else { u32::MAX << (32 - prefix) };
    format!("{}.{}.{}.{}",
            (mask >> 24) & 0xff,
            (mask >> 16) & 0xff,
            (mask >> 8) & 0xff,
            mask & 0xff)
}

fn netmask_to_prefix(value: &str) -> Option<i32> {
    let address: Ipv4Addr = value.parse().ok()?;
    let mask = u32::from(address);
    let prefix = mask.leading_ones() as i32;
    let expected = if prefix == 0 { 0u32 } else { u32::MAX << (32 - prefix) };
    if mask == expected { Some(prefix) } else { None }
}

fn open_system_keyboard(
    ui: &HomeWindow,
    title: &str,
    purpose: i32,
    return_view: i32,
    input_kind: &str,
    initial: &str,
) {
    ui.set_keyboard_title(title.into());
    ui.set_keyboard_purpose(purpose);
    ui.set_keyboard_return_view(return_view);
    ui.set_keyboard_input_kind(input_kind.into());
    ui.set_keyboard_secret(input_kind == "password");
    ui.set_keyboard_value(initial.into());
    ui.set_keyboard_page(0);
    ui.set_keyboard_index(0);
    ui.set_keyboard_shift(false);
    apply_keyboard_layout(ui);
    navigate_settings_view(ui, 6);
    ui.invoke_focus_system_keyboard();
    ui.invoke_system_keyboard_place_cursor(initial.len() as i32);
}

fn keyboard_row(ui: &HomeWindow, row: i32) -> ModelRc<SharedString> {
    match row.rem_euclid(4) {
        0 => ui.get_keyboard_row_zero(),
        1 => ui.get_keyboard_row_one(),
        2 => ui.get_keyboard_row_two(),
        _ => ui.get_keyboard_row_three(),
    }
}

fn keyboard_key(ui: &HomeWindow) -> Option<String> {
    let index = ui.get_keyboard_index().clamp(0, 39);
    let row = index / 10;
    let col = index % 10;
    keyboard_row(ui, row)
        .row_data(col as usize)
        .map(|value| value.to_string())
}

fn move_keyboard_horizontal(ui: &HomeWindow, delta: i32) {
    let index = ui.get_keyboard_index().clamp(0, 39);
    let row = index / 10;
    let model = keyboard_row(ui, row);
    let count = model.row_count() as i32;
    if count <= 0 {
        return;
    }
    let col = (index % 10).min(count - 1);
    ui.set_keyboard_index(row * 10 + (col + delta).rem_euclid(count));
}

fn move_keyboard_vertical(ui: &HomeWindow, delta: i32) {
    let index = ui.get_keyboard_index().clamp(0, 39);
    let current_row = index / 10;
    let col = index % 10;

    for step in 1..=4 {
        let row = (current_row + delta * step).rem_euclid(4);
        let count = keyboard_row(ui, row).row_count() as i32;
        if count > 0 {
            ui.set_keyboard_index(row * 10 + col.min(count - 1));
            return;
        }
    }
}

fn valid_ipv4(value: &str, allow_empty: bool) -> bool {
    (allow_empty && value.is_empty()) || value.parse::<Ipv4Addr>().is_ok()
}

fn valid_dns(value: &str) -> bool {
    if value.trim().is_empty() {
        return true;
    }
    value
        .split(|c: char| c == ',' || c.is_ascii_whitespace())
        .filter(|part| !part.is_empty())
        .all(|part| part.parse::<Ipv4Addr>().is_ok())
}

fn finish_system_keyboard(ui: &HomeWindow) {
    let purpose = ui.get_keyboard_purpose();
    let value = ui.get_keyboard_value().to_string();
    let return_view = ui.get_keyboard_return_view();

    match purpose {
        0 => {
            let ssid = ui.get_wifi_selected_ssid().to_string();
            let hidden = ui.get_wifi_pending_hidden();
            if ssid.is_empty() {
                wifi_notice(ui, tr(ui, 262, "SSID is required"));
                return;
            }
            if !value.is_empty() && value.len() < 8 {
                wifi_notice(ui, tr(ui, 263, "Password must contain at least 8 characters"));
                return;
            }
            navigate_settings_view(ui, return_view);
            ui.set_keyboard_value("".into());
            ui.set_wifi_pending_hidden(false);
            wifi_connect(ui, ssid, value, hidden);
        }
        1 => {
            if value.is_empty() {
                wifi_notice(ui, tr(ui, 262, "SSID is required"));
                return;
            }
            ui.set_wifi_selected_ssid(value.clone().into());
            ui.set_wifi_selected_security(tr(ui, 264, "Hidden").into());
            ui.set_wifi_selected_saved(false);
            ui.set_wifi_selected_current(false);
            ui.set_wifi_pending_hidden(true);
            open_system_keyboard(ui, &format!("Password • {value}"), 0, 2, "password", "");
        }
        2 => {
            if !valid_ipv4(&value, false) {
                wifi_notice(ui, tr(ui, 265, "Enter a valid IPv4 address"));
                return;
            }
            ui.set_wifi_ip_address(value.into());
            navigate_settings_view(&ui, 4);
        }
        3 => {
            let Ok(prefix) = value.parse::<i32>() else {
                wifi_notice(ui, tr(ui, 266, "Prefix must be between 0 and 32"));
                return;
            };
            if !(0..=32).contains(&prefix) {
                wifi_notice(ui, tr(ui, 266, "Prefix must be between 0 and 32"));
                return;
            }
            ui.set_wifi_ip_prefix(prefix);
            ui.set_wifi_ip_netmask(prefix_to_netmask(prefix).into());
            navigate_settings_view(&ui, 4);
        }
        4 => {
            if !valid_ipv4(&value, true) {
                wifi_notice(ui, tr(ui, 267, "Gateway must be empty or a valid IPv4 address"));
                return;
            }
            ui.set_wifi_ip_gateway(value.into());
            navigate_settings_view(&ui, 4);
        }
        5 => {
            if !valid_dns(&value) {
                wifi_notice(ui, tr(ui, 268, "DNS must contain IPv4 addresses separated by comma or space"));
                return;
            }
            ui.set_wifi_ip_dns(value.into());
            navigate_settings_view(&ui, 4);
        }
        6 => {
            let Some(prefix) = netmask_to_prefix(&value) else {
                wifi_notice(ui, tr(ui, 269, "Enter a contiguous IPv4 subnet mask"));
                return;
            };
            ui.set_wifi_ip_prefix(prefix);
            ui.set_wifi_ip_netmask(value.into());
            navigate_settings_view(&ui, 4);
        }
        7 => {
            if value.is_empty() || value.len() > 16 {
                bluetooth_notice(ui, tr(ui, 270, "PIN/passkey must contain 1 to 16 characters"));
                return;
            }
            let address = ui.get_bluetooth_selected_address().to_string();
            navigate_settings_view(ui, return_view);
            ui.set_keyboard_value("".into());
            bluetooth_pair_with_pin(ui, address, value);
        }
        8 => { if value.trim().is_empty(){return;} let id=ui.get_profile_edit_user_id().to_string();let name=value.trim().to_owned();navigate_settings_view(ui,return_view);ui.set_keyboard_value("".into());thread::spawn(move||{let _=users_command(&format!("SET_NAME	{}	{}",id,name));}); }
        9 => {
            if value.trim().is_empty(){return;}
            let name=value.trim().to_owned();
            navigate_settings_view(ui,return_view);
            ui.set_keyboard_value("".into());
            /* A new user also needs a profile picture: open the picker on it. */
            let w=ui.as_weak();
            thread::spawn(move||{
                let id=users_command(&format!("CREATE\t{}",name)).ok()
                    .and_then(|r|r.trim().strip_prefix("OK ").map(str::to_owned));
                if let Some(id)=id{
                    let _=slint::invoke_from_event_loop(move||if let Some(ui)=w.upgrade(){
                        ui.set_profile_edit_user_id(id.into());
                        ui.set_profile_edit_name(name.into());
                        open_avatar_picker(&ui);
                    });
                }
            });
        }
        _ => navigate_settings_view(ui, return_view),
    }
}

fn handle_system_keyboard_confirm(ui: &HomeWindow) {
    let Some(key) = keyboard_key(ui) else { return; };
    match key.as_str() {
        "Back" => {
            let mut value = ui.get_keyboard_value().to_string();
            value.pop();
            let cursor = value.len() as i32;
            ui.set_keyboard_value(value.into());
            ui.invoke_system_keyboard_place_cursor(cursor);
        }
        "Space" => {
            let mut value = ui.get_keyboard_value().to_string();
            value.push(' ');
            let cursor = value.len() as i32;
            ui.set_keyboard_value(value.into());
            ui.invoke_system_keyboard_place_cursor(cursor);
        }
        "Shift" => {
            ui.set_keyboard_shift(!ui.get_keyboard_shift());
            apply_keyboard_layout(ui);
        },
        "123" => {
            ui.set_keyboard_page(1);
            ui.set_keyboard_shift(false);
            ui.set_keyboard_index(0);
            apply_keyboard_layout(ui);
        }
        "SYM" => {
            ui.set_keyboard_page(2);
            ui.set_keyboard_shift(false);
            ui.set_keyboard_index(0);
            apply_keyboard_layout(ui);
        }
        "ABC" => {
            ui.set_keyboard_page(0);
            ui.set_keyboard_shift(false);
            ui.set_keyboard_index(0);
            apply_keyboard_layout(ui);
        }
        "Done" => finish_system_keyboard(ui),
        _ => {
            let kind = ui.get_keyboard_input_kind().to_string();
            let allowed = match kind.as_str() {
                "ipv4" => key.chars().all(|c| c.is_ascii_digit() || c == '.'),
                "number" => key.chars().all(|c| c.is_ascii_digit()),
                "dns" => key.chars().all(|c| c.is_ascii_digit() || c == '.' || c == ','),
                _ => true,
            };
            if !allowed {
                return;
            }
            let mut value = ui.get_keyboard_value().to_string();
            if ui.get_keyboard_shift() && key.chars().all(|c| c.is_alphabetic()) {
                value.push_str(&key.to_uppercase());
                ui.set_keyboard_shift(false);
                apply_keyboard_layout(ui);
            } else {
                value.push_str(&key);
            }
            let cursor = value.len() as i32;
            ui.set_keyboard_value(value.into());
            ui.invoke_system_keyboard_place_cursor(cursor);
        }
    }
}

fn open_network_detail(ui: &HomeWindow, network: WifiNetworkEntry, return_view: i32) {
    let ssid = network.ssid.to_string();
    ui.set_wifi_selected_ssid(network.ssid.clone());
    ui.set_wifi_selected_security(network.security.clone());
    ui.set_wifi_selected_saved(network.saved);
    ui.set_wifi_selected_current(network.current);
    ui.set_wifi_detail_index(0);
    ui.set_wifi_detail_scroll_offset(0);
    ui.set_wifi_detail_return_view(return_view);
    navigate_settings_view(&ui, 4);
    wifi_notice(ui, "");
    fetch_wifi_ip_configuration(ui, ssid);
}

fn move_model_selection(current: i32, count: i32, delta: i32) -> i32 {
    if count <= 0 {
        return 0;
    }
    (current + delta).rem_euclid(count)
}

fn update_wifi_menu_scroll(ui: &HomeWindow) {
    let visible_rows = ui.get_settings_list_visible_rows().max(1);
    let index = ui.get_wifi_menu_index().max(0);
    let mut offset = ui.get_wifi_menu_scroll_offset().max(0);
    if index < offset {
        offset = index;
    } else if index >= offset + visible_rows {
        offset = index - visible_rows + 1;
    }
    ui.set_wifi_menu_scroll_offset(offset.max(0));
}

fn update_scroll_offset(ui: &HomeWindow, saved: bool) {
    let index = if saved { ui.get_wifi_saved_index() } else { ui.get_wifi_network_index() };
    let count = if saved {
        ui.get_wifi_saved_networks().row_count() as i32
    } else {
        // Connect view includes Add Hidden Network at selection index 0.
        ui.get_wifi_networks().row_count() as i32 + 1
    };
    let current = if saved {
        ui.get_wifi_saved_scroll_offset()
    } else {
        ui.get_wifi_network_scroll_offset()
    };
    let offset = guarded_scroll_offset(index, count, ui.get_settings_list_visible_rows(), current);

    if saved {
        ui.set_wifi_saved_scroll_offset(offset);
    } else {
        ui.set_wifi_network_scroll_offset(offset);
    }
}

fn update_wifi_detail_scroll(ui: &HomeWindow) {
    const DETAIL_ROWS: i32 = 9;
    ui.set_wifi_detail_scroll_offset(guarded_scroll_offset(
        ui.get_wifi_detail_index(),
        DETAIL_ROWS,
        ui.get_wifi_detail_visible_rows(),
        ui.get_wifi_detail_scroll_offset(),
    ));
}

fn start_wifi_aux_signal_listener(ui: &HomeWindow, signal_name: &'static str) {
    let weak = ui.as_weak();
    thread::spawn(move || loop {
        let connection = match zbus::blocking::Connection::system() {
            Ok(connection) => connection,
            Err(error) => {
                eprintln!("home: Wi-Fi auxiliary bus failed={error}");
                thread::sleep(Duration::from_millis(500));
                continue;
            }
        };
        let proxy = match wifi_proxy(&connection) {
            Ok(proxy) => proxy,
            Err(error) => {
                eprintln!("home: Wi-Fi auxiliary proxy failed={error}");
                thread::sleep(Duration::from_millis(500));
                continue;
            }
        };
        let mut signals = match proxy.receive_signal(signal_name) {
            Ok(signals) => signals,
            Err(error) => {
                eprintln!("home: Wi-Fi {signal_name} subscription failed={error}");
                thread::sleep(Duration::from_millis(500));
                continue;
            }
        };
        for message in &mut signals {
            match signal_name {
                "NetworksSnapshotChanged" => {
                    let body = message
                        .body()
                        .deserialize::<(Vec<(String, String, i32, bool, bool)>,)>();
                    match body {
                        Ok((rows,)) => {
                            let weak = weak.clone();
                            let _ = slint::invoke_from_event_loop(move || {
                                if let Some(ui) = weak.upgrade() {
                                    apply_wifi_network_rows(&ui, rows, false);
                                }
                            });
                        }
                        Err(error) => {
                            eprintln!("home: invalid Wi-Fi NetworksSnapshotChanged payload={error}");
                        }
                    }
                }
                "NetworksChanged" => {
                    let weak = weak.clone();
                    let _ = slint::invoke_from_event_loop(move || {
                        if let Some(ui) = weak.upgrade() {
                            refresh_wifi_networks(&ui, true);
                        }
                    });
                }
                "ScanStateChanged" => {
                    let body = message.body().deserialize::<(bool,)>();
                    if let Ok((scanning,)) = body {
                        let weak = weak.clone();
                        let _ = slint::invoke_from_event_loop(move || {
                            if let Some(ui) = weak.upgrade() {
                                ui.set_wifi_scanning(scanning);
                                if !scanning {
                                    wifi_notice(&ui, "");
                                    refresh_wifi_networks(&ui, false);
                                }
                            }
                        });
                    }
                }
                "OperationFailed" => {
                    let body = message.body().deserialize::<(String, String)>();
                    if let Ok((operation, text)) = body {
                        let weak = weak.clone();
                        let _ = slint::invoke_from_event_loop(move || {
                            if let Some(ui) = weak.upgrade() {
                                wifi_notice(&ui, format!("{operation}: {text}"));
                            }
                        });
                    }
                }
                "IpConfigurationChanged" => {
                    let body = message.body().deserialize::<(String,)>();
                    if let Ok((ssid,)) = body {
                        let weak = weak.clone();
                        let _ = slint::invoke_from_event_loop(move || {
                            if let Some(ui) = weak.upgrade() {
                                if ui.get_settings_view() == 4 && ui.get_wifi_selected_ssid().as_str() == ssid {
                                    fetch_wifi_ip_configuration(&ui, ssid);
                                }
                            }
                        });
                    }
                }
                _ => {}
            }
        }
        thread::sleep(Duration::from_millis(250));
    });
}

fn start_wifi_product_listener(ui: &HomeWindow) {
    start_wifi_aux_signal_listener(ui, "NetworksSnapshotChanged");
    start_wifi_aux_signal_listener(ui, "NetworksChanged");
    start_wifi_aux_signal_listener(ui, "ScanStateChanged");
    start_wifi_aux_signal_listener(ui, "OperationFailed");
    start_wifi_aux_signal_listener(ui, "IpConfigurationChanged");

    let weak = ui.as_weak();
    thread::spawn(move || loop {
        let connection = match zbus::blocking::Connection::system() {
            Ok(connection) => connection,
            Err(error) => {
                eprintln!("home: Wi-Fi system bus connection failed={error}");
                thread::sleep(Duration::from_millis(500));
                continue;
            }
        };

        let proxy = match wifi_proxy(&connection) {
            Ok(proxy) => proxy,
            Err(error) => {
                eprintln!("home: Wi-Fi product proxy failed={error}");
                thread::sleep(Duration::from_millis(500));
                continue;
            }
        };

        let initial_snapshot: zbus::Result<(bool, String, String, String, i32)> =
            proxy.call("GetSnapshot", &());
        if let Ok((enabled, state, ssid, ipv4, signal_dbm)) = initial_snapshot {
            let snapshot = WifiProductState { enabled, state, ssid, ipv4, signal_dbm };
            let weak = weak.clone();
            let _ = slint::invoke_from_event_loop(move || {
                if let Some(ui) = weak.upgrade() {
                    apply_wifi_product_state(&ui, snapshot);
                }
            });
        }

        let mut signals = match proxy.receive_signal("StateChanged") {
            Ok(signals) => signals,
            Err(error) => {
                eprintln!("home: Wi-Fi signal subscription failed={error}");
                thread::sleep(Duration::from_millis(500));
                continue;
            }
        };

        for message in &mut signals {
            let body = message.body();
            let snapshot = body.deserialize::<(bool, String, String, String, i32)>();
            let Ok((enabled, state, ssid, ipv4, signal_dbm)) = snapshot else {
                eprintln!("home: invalid Wi-Fi StateChanged payload");
                continue;
            };
            let snapshot = WifiProductState { enabled, state, ssid, ipv4, signal_dbm };
            let weak = weak.clone();
            let _ = slint::invoke_from_event_loop(move || {
                if let Some(ui) = weak.upgrade() {
                    apply_wifi_product_state(&ui, snapshot);
                    refresh_wifi_networks(&ui, false);
                    refresh_wifi_networks(&ui, true);
                }
            });
        }
        thread::sleep(Duration::from_millis(250));
    });
}

fn start_bluetooth_aux_signal_listener(ui: &HomeWindow, signal_name: &'static str) {
    let weak = ui.as_weak();
    thread::spawn(move || loop {
        let connection = match zbus::blocking::Connection::system() {
            Ok(connection) => connection,
            Err(error) => {
                eprintln!("home: Bluetooth auxiliary bus failed={error}");
                thread::sleep(Duration::from_millis(500));
                continue;
            }
        };
        let proxy = match bluetooth_proxy(&connection) {
            Ok(proxy) => proxy,
            Err(error) => {
                eprintln!("home: Bluetooth auxiliary proxy failed={error}");
                thread::sleep(Duration::from_millis(500));
                continue;
            }
        };
        let mut signals = match proxy.receive_signal(signal_name) {
            Ok(signals) => signals,
            Err(error) => {
                eprintln!("home: Bluetooth {signal_name} subscription failed={error}");
                thread::sleep(Duration::from_millis(500));
                continue;
            }
        };

        for message in &mut signals {
            match signal_name {
                "DevicesSnapshotChanged" => {
                    let body = message
                        .body()
                        .deserialize::<(Vec<(String, String, String, bool, bool, bool, i32)>,)>();
                    match body {
                        Ok((rows,)) => {
                            let weak = weak.clone();
                            let _ = slint::invoke_from_event_loop(move || {
                                if let Some(ui) = weak.upgrade() {
                                    apply_bluetooth_device_rows(&ui, rows);
                                }
                            });
                        }
                        Err(error) => eprintln!("home: invalid Bluetooth device snapshot={error}"),
                    }
                }
                "DiscoveryStateChanged" => {
                    let body = message.body().deserialize::<(bool,)>();
                    if let Ok((discovering,)) = body {
                        let weak = weak.clone();
                        let _ = slint::invoke_from_event_loop(move || {
                            if let Some(ui) = weak.upgrade() {
                                ui.set_bluetooth_scanning(discovering);
                            }
                        });
                    }
                }
                _ => {}
            }
        }

        thread::sleep(Duration::from_millis(250));
    });
}

fn start_bluetooth_product_listener(ui: &HomeWindow) {
    start_bluetooth_aux_signal_listener(ui, "DevicesSnapshotChanged");
    start_bluetooth_aux_signal_listener(ui, "DiscoveryStateChanged");

    let weak = ui.as_weak();
    thread::spawn(move || loop {
        let connection = match zbus::blocking::Connection::system() {
            Ok(connection) => connection,
            Err(error) => {
                eprintln!("home: Bluetooth system bus connection failed={error}");
                thread::sleep(Duration::from_millis(500));
                continue;
            }
        };

        let proxy = match bluetooth_proxy(&connection) {
            Ok(proxy) => proxy,
            Err(error) => {
                eprintln!("home: Bluetooth product proxy failed={error}");
                thread::sleep(Duration::from_millis(500));
                continue;
            }
        };

        let initial_snapshot: zbus::Result<(bool, bool, String, u32, u32)> =
            proxy.call("GetSnapshot", &());

        match initial_snapshot {
            Ok((present, powered, address, connected_count, paired_count)) => {
                let snapshot = BluetoothProductState {
                    present,
                    powered,
                    address,
                    connected_count,
                    paired_count,
                };
                let weak = weak.clone();
                let _ = slint::invoke_from_event_loop(move || {
                    if let Some(ui) = weak.upgrade() {
                        apply_bluetooth_product_state(&ui, snapshot);
                        refresh_bluetooth_devices(&ui);
                    }
                });
            }
            Err(error) => eprintln!("home: Bluetooth snapshot failed={error}"),
        }

        let mut signals = match proxy.receive_signal("StateChanged") {
            Ok(signals) => signals,
            Err(error) => {
                eprintln!("home: Bluetooth signal subscription failed={error}");
                thread::sleep(Duration::from_millis(500));
                continue;
            }
        };

        for message in &mut signals {
            let body = message.body();
            let snapshot = body.deserialize::<(bool, bool, String, u32, u32)>();
            let Ok((present, powered, address, connected_count, paired_count)) = snapshot else {
                eprintln!("home: invalid Bluetooth StateChanged payload");
                continue;
            };
            let snapshot = BluetoothProductState {
                present,
                powered,
                address,
                connected_count,
                paired_count,
            };
            let weak = weak.clone();
            let _ = slint::invoke_from_event_loop(move || {
                if let Some(ui) = weak.upgrade() {
                    apply_bluetooth_product_state(&ui, snapshot);
                }
            });
        }

        thread::sleep(Duration::from_millis(250));
    });
}

fn load_topbar_state() -> TopbarState {
    let mut state = TopbarState::default();
    let Ok(contents) = fs::read_to_string(STATUS_STATE) else {
        return state;
    };

    for line in contents.lines() {
        let Some((key, value)) = line.split_once('=') else {
            continue;
        };

        match key {
            "TIME" => state.time = value.to_owned(),
            "USER_NAME" => state.user_name = value.to_owned(),
            "WIFI_STATE" => state.wifi_state = value.to_owned(),
            "BATTERY_PERCENT" => {
                state.battery_percent = value.parse::<i32>().unwrap_or(-1);
            }
            "BATTERY_STATE" => state.battery_state = value.to_owned(),
            _ => {}
        }
    }

    state
}

fn apply_topbar_state(ui: &HomeWindow, state: TopbarState) {
    ui.set_topbar_time(state.time.into());
    ui.set_current_user_name(state.user_name.into());
    ui.set_wifi_state(state.wifi_state.into());
    ui.set_battery_percent(state.battery_percent);
    ui.set_battery_state(state.battery_state.into());
    ui.set_battery_label(if state.battery_percent >= 0 { format!("{}%", state.battery_percent).into() } else { "--%".into() });
}

fn apply_latest_topbar_state(weak: &slint::Weak<HomeWindow>) -> bool {
    let state = load_topbar_state();
    let weak = weak.clone();
    slint::invoke_from_event_loop(move || {
        if let Some(ui) = weak.upgrade() {
            apply_topbar_state(&ui, state);
        }
    })
    .is_ok()
}

fn start_status_listener(ui: &HomeWindow) {
    apply_topbar_state(ui, load_topbar_state());

    let weak = ui.as_weak();
    thread::spawn(move || loop {
        match UnixStream::connect(STATUS_SOCKET) {
            Ok(stream) => {
                eprintln!("home: status service connected");
                let mut reader = BufReader::new(stream);
                let mut line = String::new();

                loop {
                    line.clear();
                    match reader.read_line(&mut line) {
                        Ok(0) => {
                            eprintln!("home: status stream EOF");
                            break;
                        }
                        Ok(_) if line.trim() == "changed" => {
                            /* Same event contract as Quick Menu: statusd emits
                             * changed only after atomically publishing the new
                             * state file, then Home reloads that exact snapshot. */
                            if !apply_latest_topbar_state(&weak) {
                                return;
                            }
                        }
                        Ok(_) => {}
                        Err(error) => {
                            eprintln!("home: status read failed={error}");
                            break;
                        }
                    }
                }
            }
            Err(error) => eprintln!("home: status service unavailable={error}"),
        }

        if !apply_latest_topbar_state(&weak) {
            return;
        }
        thread::sleep(Duration::from_millis(250));
    });
}


fn input_command(command: &str) -> std::io::Result<String> {
    let mut stream = UnixStream::connect(INPUT_SOCKET)?;
    stream.write_all(command.as_bytes())?;
    stream.write_all(b"\n")?;

    let mut reader = BufReader::new(stream);
    let mut reply = String::new();
    reader.read_line(&mut reply)?;
    Ok(reply)
}

/* Controller navigation capture for the foreground nuubUI (Home, library,
 * Settings, OOB). inputd still calls it the Settings capture; it is
 * released only when the UI exits (later: while a game/app runs). */
fn set_ui_capture(enabled: bool) {
    let command = if enabled {
        "SETTINGS OPEN"
    } else {
        "SETTINGS CLOSE"
    };

    if let Err(error) = input_command(command) {
        eprintln!(
            "home: settings capture enabled={} failed={}",
            enabled, error
        );
    }
}

fn binding_code(action: &str) -> Option<i32> {
    let reply = input_command(&format!("BIND GET {}", action)).ok()?;
    reply.trim().parse::<i32>().ok()
}

/*
 * Linux BTN_* semantic face positions, not Xbox/Nintendo letters:
 * 0 top, 1 right, 2 bottom, 3 left.
 */
fn face_position(code: i32) -> i32 {
    match code {
        307 => 0, // BTN_NORTH
        305 => 1, // BTN_EAST
        304 => 2, // BTN_SOUTH
        308 => 3, // BTN_WEST
        _ => -1,
    }
}

fn refresh_hint_mapping(ui: &HomeWindow) {
    ui.set_select_face_position(
        binding_code("menu_confirm")
            .map(face_position)
            .unwrap_or(-1),
    );
    ui.set_back_face_position(
        binding_code("menu_back")
            .map(face_position)
            .unwrap_or(-1),
    );
    ui.set_context_face_position(
        binding_code("face_north")
            .map(face_position)
            .unwrap_or(-1),
    );
}

fn update_display_audio_scroll(ui: &HomeWindow, count: i32) {
    ui.set_display_audio_scroll_offset(guarded_scroll_offset(
        ui.get_display_audio_index(),
        count,
        ui.get_settings_list_visible_rows(),
        ui.get_display_audio_scroll_offset(),
    ));
}

fn update_system_scroll(ui: &HomeWindow) {
    const COUNT: i32 = 8;
    ui.set_system_scroll_offset(guarded_scroll_offset(
        ui.get_system_index(),
        COUNT,
        ui.get_settings_list_visible_rows(),
        ui.get_system_scroll_offset(),
    ));
}

fn update_system_storage_scroll(ui: &HomeWindow) {
    const COUNT: i32 = 7;
    ui.set_system_storage_scroll_offset(guarded_scroll_offset(
        ui.get_system_storage_index(),
        COUNT,
        ui.get_settings_list_visible_rows(),
        ui.get_system_storage_scroll_offset(),
    ));
}

fn update_system_info_scroll(ui: &HomeWindow) {
    const COUNT: i32 = 10;
    ui.set_system_info_scroll_offset(guarded_scroll_offset(
        ui.get_system_info_index(),
        COUNT,
        ui.get_settings_list_visible_rows(),
        ui.get_system_info_scroll_offset(),
    ));
}

fn handle_settings_action(
    ui: &HomeWindow,
    action: &str,
    settings_active: &Arc<AtomicBool>,
) {
    play_ui_sound(action);
    let view = ui.get_settings_view();
    if ui.get_avatar_picker_open(){if action=="menu_back"{ui.set_avatar_picker_open(false);return;}let n=ui.get_avatar_choices().row_count() as i32;match action{"menu_up" if n>0=>{ui.set_avatar_picker_index(move_model_selection(ui.get_avatar_picker_index(),n,-1));update_avatar_scroll(ui);},"menu_down" if n>0=>{ui.set_avatar_picker_index(move_model_selection(ui.get_avatar_picker_index(),n,1));update_avatar_scroll(ui);},"menu_confirm" if n>0=>{if let Some(a)=ui.get_avatar_choices().row_data(ui.get_avatar_picker_index().max(0) as usize){let id=ui.get_profile_edit_user_id().to_string();let spec=a.spec.to_string();ui.set_avatar_picker_open(false);thread::spawn(move||{let _=users_command(&format!("SET_AVATAR	{}	{}",id,spec));});}},_=>{}}return;}

    if ui.get_settings_choice_open() {
        if action == "menu_back" {
            ui.set_settings_choice_open(false);
            return;
        }

        let count = ui.get_settings_choice_options().row_count() as i32;
        match action {
            "menu_up" if count > 0 => {
                ui.set_settings_choice_index(
                    move_model_selection(ui.get_settings_choice_index(), count, -1)
                );
                update_settings_choice_scroll(ui);
            }
            "menu_down" if count > 0 => {
                ui.set_settings_choice_index(
                    move_model_selection(ui.get_settings_choice_index(), count, 1)
                );
                update_settings_choice_scroll(ui);
            }
            "menu_confirm" if count > 0 => apply_settings_choice(ui),
            _ => {}
        }
        return;
    }
    if action == "menu_back" && view == 17 && ui.get_lighting_preview_open() {
        ui.set_lighting_preview_open(false);
        lighting_stop_effect(ui);
        return;
    }

    if action == "menu_back" {
        match view {
            0 => {
                settings_active.store(false, Ordering::SeqCst);
                ui.set_settings_open(false);
                write_ui_context("home");
            }
            1 => navigate_settings_view(ui, 0),
            2 | 3 => navigate_settings_view(ui, 1),
            4 => navigate_settings_view(ui, ui.get_wifi_detail_return_view()),
            6 => navigate_settings_view(ui, ui.get_keyboard_return_view()),
            7 | 8 => navigate_settings_view(ui, 1),
            9 => navigate_settings_view(ui, ui.get_bluetooth_detail_return_view()),
            10 => navigate_settings_view(ui, 0),
            11 | 12 | 16 | 17 => navigate_settings_view(ui, 10),
            18 => {
                audio_stop_test_tone(ui);
                navigate_settings_view(ui, 0);
            }
            19 | 23 => navigate_settings_view(ui, 0),
            24 | 25 => navigate_settings_view(ui, 23),
            26 => navigate_settings_view(ui, 25),
            20 | 21 | 22 => {
                ui.set_system_reset_confirm(false);
                ui.set_system_storage_confirm(false);
                navigate_settings_view(ui, 19);
            }
            13 => navigate_settings_view(ui, 12),
            15 => navigate_settings_view(ui, 13),
            14 => navigate_settings_view(ui, ui.get_controller_tester_return_view()),
            _ => navigate_settings_view(ui, 0),
        }
        return;
    }

    match view {
        0 => match action {
            "menu_up" => {
                let current = ui.get_settings_selected_index();
                let next = if current <= 0 { 4 } else { current - 1 };
                ui.set_settings_selected_index(next);
                if next == 3 {
                    /* The category rail already renders the real Display & Audio
                     * page, so refresh the exact model shown when focus moves here. */
                    ui.set_display_audio_scroll_offset(0);
                    refresh_display_state(ui);
                    refresh_audio(ui);
                    refresh_audio_policy(ui);
                } else if next == 4 {
                    ui.set_system_scroll_offset(0);
                    refresh_system(ui);
                }
            }
            "menu_down" => {
                let current = ui.get_settings_selected_index();
                let next = if current >= 4 { 0 } else { current + 1 };
                ui.set_settings_selected_index(next);
                if next == 3 {
                    ui.set_display_audio_scroll_offset(0);
                    refresh_display_state(ui);
                    refresh_audio(ui);
                    refresh_audio_policy(ui);
                } else if next == 4 {
                    ui.set_system_scroll_offset(0);
                    refresh_system(ui);
                }
            }
            "menu_confirm" => {
                if ui.get_settings_selected_index() == 0 {
                    ui.set_general_index(0);
                    ui.set_general_scroll_offset(0);
                    navigate_settings_view(ui, 23);
                } else if ui.get_settings_selected_index() == 1 {
                    ui.set_connectivity_selected_index(0);
                    navigate_settings_view(ui, 1);
                    wifi_notice(ui, "");
                    refresh_wifi_networks(ui, true);
                } else if ui.get_settings_selected_index() == 2 {
                    ui.set_controllers_menu_index(0);
                    navigate_settings_view(ui, 10);
                } else if ui.get_settings_selected_index() == 3 {
                    ui.set_display_audio_index(0);
                    ui.set_display_audio_scroll_offset(0);
                    navigate_settings_view(ui, 18);
                    refresh_display_state(ui);
                    refresh_audio(ui);
                    refresh_audio_policy(ui);
                } else if ui.get_settings_selected_index() == 4 {
                    ui.set_system_index(0);
                    ui.set_system_scroll_offset(0);
                    navigate_settings_view(ui, 19);
                    refresh_system(ui);
                }
            }
            _ => {}
        },
        1 => match action {
            "menu_up" => {
                let current = ui.get_connectivity_selected_index();
                ui.set_connectivity_selected_index((current + 5).rem_euclid(6));
            }
            "menu_down" => {
                let current = ui.get_connectivity_selected_index();
                ui.set_connectivity_selected_index((current + 1).rem_euclid(6));
            }
            "menu_confirm" => match ui.get_connectivity_selected_index() {
                0 => wifi_set_enabled(ui, !ui.get_wifi_enabled()),
                1 if ui.get_wifi_enabled() => {
                    navigate_settings_view(ui, 2);
                }
                2 => {
                    ui.set_wifi_saved_index(0);
                    ui.set_wifi_saved_scroll_offset(0);
                    navigate_settings_view(ui, 3);
                    refresh_wifi_networks(ui, true);
                }
                3 => bluetooth_set_enabled(ui, !ui.get_bluetooth_enabled()),
                4 if ui.get_bluetooth_enabled() => {
                    navigate_settings_view(ui, 7);
                }
                5 if ui.get_bluetooth_enabled() => {
                    ui.set_bluetooth_known_index(0);
                    ui.set_bluetooth_known_scroll_offset(0);
                    refresh_bluetooth_devices(ui);
                    navigate_settings_view(ui, 8);
                }
                _ => {}
            },
            _ => {}
        },
        2 => {
            let network_count = ui.get_wifi_networks().row_count() as i32;
            let total = 1 + network_count;
            match action {
                "menu_up" if total > 0 => {
                    ui.set_wifi_network_index(move_model_selection(ui.get_wifi_network_index(), total, -1));
                    update_scroll_offset(ui, false);
                }
                "menu_down" if total > 0 => {
                    ui.set_wifi_network_index(move_model_selection(ui.get_wifi_network_index(), total, 1));
                    update_scroll_offset(ui, false);
                }
                "menu_confirm" => {
                    let index = ui.get_wifi_network_index();
                    if index == 0 {
                        ui.set_wifi_pending_hidden(true);
                        open_system_keyboard(ui, "Hidden Network SSID", 1, 2, "text", "");
                    } else if let Some(network) = ui.get_wifi_networks().row_data((index - 1) as usize) {
                        open_network_detail(ui, network, 2);
                    }
                }
                _ => {}
            }
        }
        3 => {
            let count = ui.get_wifi_saved_networks().row_count() as i32;
            match action {
                "menu_up" if count > 0 => {
                    ui.set_wifi_saved_index(move_model_selection(ui.get_wifi_saved_index(), count, -1));
                    update_scroll_offset(ui, true);
                }
                "menu_down" if count > 0 => {
                    ui.set_wifi_saved_index(move_model_selection(ui.get_wifi_saved_index(), count, 1));
                    update_scroll_offset(ui, true);
                }
                "menu_confirm" if count > 0 => {
                    if let Some(network) = ui.get_wifi_saved_networks().row_data(ui.get_wifi_saved_index() as usize) {
                        open_network_detail(ui, network, 3);
                    }
                }
                _ => {}
            }
        }
        4 => match action {
            "menu_up" => {
                ui.set_wifi_detail_index((ui.get_wifi_detail_index() + 8).rem_euclid(9));
                update_wifi_detail_scroll(ui);
            }
            "menu_down" => {
                ui.set_wifi_detail_index((ui.get_wifi_detail_index() + 1).rem_euclid(9));
                update_wifi_detail_scroll(ui);
            }
            "menu_left" | "menu_right" if ui.get_wifi_detail_index() == 2 => {
                let manual = ui.get_wifi_ip_mode().as_str() != "manual";
                if manual {
                    ui.set_wifi_ip_mode("manual".into());
                    if ui.get_wifi_ip_address().is_empty() {
                        ui.set_wifi_ip_address(ui.get_wifi_runtime_address());
                        ui.set_wifi_ip_prefix(ui.get_wifi_runtime_prefix());
                        ui.set_wifi_ip_netmask(ui.get_wifi_runtime_netmask());
                        ui.set_wifi_ip_gateway(ui.get_wifi_runtime_gateway());
                        ui.set_wifi_ip_dns(ui.get_wifi_runtime_dns());
                    }
                } else {
                    ui.set_wifi_ip_mode("automatic".into());
                }
            }
            "menu_confirm" => match ui.get_wifi_detail_index() {
                0 if ui.get_wifi_selected_current() => wifi_disconnect(ui),
                0 if matches!(ui.get_wifi_selected_security().as_str(), "Enterprise" | "WEP") => {
                    wifi_notice(ui, tr(ui, 271, "This network security mode is not supported"));
                }
                0 if ui.get_wifi_selected_saved() => {
                    wifi_connect(ui, ui.get_wifi_selected_ssid().to_string(), String::new(), false);
                }
                0 if ui.get_wifi_selected_security().as_str() == "Open" => {
                    wifi_connect(ui, ui.get_wifi_selected_ssid().to_string(), String::new(), false);
                }
                0 => {
                    let ssid = ui.get_wifi_selected_ssid().to_string();
                    open_system_keyboard(ui, &format!("Password • {ssid}"), 0, 4, "password", "");
                }
                1 if ui.get_wifi_selected_security().as_str() != "Open" => {
                    let ssid = ui.get_wifi_selected_ssid().to_string();
                    open_system_keyboard(ui, &format!("Password • {ssid}"), 0, 4, "password", "");
                }
                2 => {
                    let manual = ui.get_wifi_ip_mode().as_str() != "manual";
                    if manual {
                        ui.set_wifi_ip_mode("manual".into());
                        if ui.get_wifi_ip_address().is_empty() {
                            ui.set_wifi_ip_address(ui.get_wifi_runtime_address());
                            ui.set_wifi_ip_prefix(ui.get_wifi_runtime_prefix());
                            ui.set_wifi_ip_netmask(ui.get_wifi_runtime_netmask());
                            ui.set_wifi_ip_gateway(ui.get_wifi_runtime_gateway());
                            ui.set_wifi_ip_dns(ui.get_wifi_runtime_dns());
                        }
                    } else {
                        ui.set_wifi_ip_mode("automatic".into());
                    }
                }
                3 if ui.get_wifi_ip_mode().as_str() == "manual" => {
                    open_system_keyboard(ui, "IPv4 Address", 2, 4, "ipv4", &ui.get_wifi_ip_address());
                }
                4 if ui.get_wifi_ip_mode().as_str() == "manual" => {
                    open_system_keyboard(ui, "Subnet Mask", 6, 4, "ipv4", &ui.get_wifi_ip_netmask());
                }
                5 if ui.get_wifi_ip_mode().as_str() == "manual" => {
                    open_system_keyboard(ui, "Gateway (optional)", 4, 4, "ipv4", &ui.get_wifi_ip_gateway());
                }
                6 if ui.get_wifi_ip_mode().as_str() == "manual" => {
                    open_system_keyboard(ui, "DNS", 5, 4, "dns", &ui.get_wifi_ip_dns());
                }
                7 => save_wifi_ip_configuration(ui),
                8 if ui.get_wifi_selected_saved() => {
                    wifi_forget(ui, ui.get_wifi_selected_ssid().to_string());
                }
                _ => {}
            },
            _ => {}
        },
        7 => {
            let count = ui.get_bluetooth_devices().row_count() as i32;
            match action {
                "menu_up" if count > 0 => {
                    ui.set_bluetooth_device_index(
                        move_model_selection(ui.get_bluetooth_device_index(), count, -1)
                    );
                    update_bluetooth_scroll(ui, false);
                }
                "menu_down" if count > 0 => {
                    ui.set_bluetooth_device_index(
                        move_model_selection(ui.get_bluetooth_device_index(), count, 1)
                    );
                    update_bluetooth_scroll(ui, false);
                }
                "menu_confirm" if count > 0 => {
                    if let Some(device) =
                        ui.get_bluetooth_devices().row_data(ui.get_bluetooth_device_index() as usize)
                    {
                        open_bluetooth_detail(ui, device, 7);
                    }
                }
                _ => {}
            }
        }
        8 => {
            let count = ui.get_bluetooth_known_devices().row_count() as i32;
            match action {
                "menu_up" if count > 0 => {
                    ui.set_bluetooth_known_index(
                        move_model_selection(ui.get_bluetooth_known_index(), count, -1)
                    );
                    update_bluetooth_scroll(ui, true);
                }
                "menu_down" if count > 0 => {
                    ui.set_bluetooth_known_index(
                        move_model_selection(ui.get_bluetooth_known_index(), count, 1)
                    );
                    update_bluetooth_scroll(ui, true);
                }
                "menu_confirm" if count > 0 => {
                    if let Some(device) =
                        ui.get_bluetooth_known_devices().row_data(ui.get_bluetooth_known_index() as usize)
                    {
                        open_bluetooth_detail(ui, device, 8);
                    }
                }
                _ => {}
            }
        }
        9 => {
            let total = 2;
            match action {
                "menu_up" => ui.set_bluetooth_detail_index(
                    move_model_selection(ui.get_bluetooth_detail_index(), total, -1)
                ),
                "menu_down" => ui.set_bluetooth_detail_index(
                    move_model_selection(ui.get_bluetooth_detail_index(), total, 1)
                ),
                "menu_confirm" => {
                    let address = ui.get_bluetooth_selected_address().to_string();
                    if ui.get_bluetooth_detail_index() == 0 {
                        if !ui.get_bluetooth_selected_paired() {
                            bluetooth_device_operation(ui, "PairAndConnect", address);
                        } else if ui.get_bluetooth_selected_connected() {
                            bluetooth_device_operation(ui, "Disconnect", address);
                        } else {
                            bluetooth_device_operation(ui, "Connect", address);
                        }
                    } else if !ui.get_bluetooth_selected_paired() {
                        open_system_keyboard(
                            ui,
                            &format!("PIN / Passkey • {}", ui.get_bluetooth_selected_name()),
                            7,
                            9,
                            "password",
                            "",
                        );
                    } else {
                        bluetooth_device_operation(ui, "Forget", address);
                    }
                }
                _ => {}
            }
        }
        10 => match action {
            "menu_up" => ui.set_controllers_menu_index(
                move_model_selection(ui.get_controllers_menu_index(), 4, -1)
            ),
            "menu_down" => ui.set_controllers_menu_index(
                move_model_selection(ui.get_controllers_menu_index(), 4, 1)
            ),
            "menu_confirm" => match ui.get_controllers_menu_index() {
                0 => {
                    ui.set_player_assignment_index(0);
                    ui.set_player_assignment_scroll_offset(0);
                    navigate_settings_view(ui, 11);
                }
                1 => {
                    ui.set_controller_list_index(0);
                    ui.set_controller_list_scroll_offset(0);
                    navigate_settings_view(ui, 12);
                }
                2 if ui.get_rumble_supported() => {
                    ui.set_rumble_index(0);
                    navigate_settings_view(ui, 16);
                }
                3 if ui.get_lighting_supported() => {
                    ui.set_lighting_index(0);
                    navigate_settings_view(ui, 17);
                }
                _ => {}
            },
            _ => {}
        },
        11 => {
            let count = ui.get_player_assignments().row_count() as i32;
            match action {
                "menu_up" if count > 0 => {
                    ui.set_player_assignment_index(
                        move_model_selection(ui.get_player_assignment_index(), count, -1),
                    );
                    update_player_assignment_scroll(ui);
                }
                "menu_down" if count > 0 => {
                    ui.set_player_assignment_index(
                        move_model_selection(ui.get_player_assignment_index(), count, 1),
                    );
                    update_player_assignment_scroll(ui);
                }
                "menu_confirm" if count > 0 => open_player_assignment_dropdown(ui),
                _ => {}
            }
        }
        12 => {
            let count = ui.get_controllers().row_count() as i32;
            match action {
                "menu_up" if count > 0 => {
                    ui.set_controller_list_index(
                        move_model_selection(ui.get_controller_list_index(), count, -1)
                    );
                    update_controller_list_scroll(ui);
                }
                "menu_down" if count > 0 => {
                    ui.set_controller_list_index(
                        move_model_selection(ui.get_controller_list_index(), count, 1)
                    );
                    update_controller_list_scroll(ui);
                }
                "menu_confirm" if count > 0 => {
                    if let Some(controller) =
                        ui.get_controllers().row_data(ui.get_controller_list_index() as usize)
                    {
                        open_controller_detail(ui, controller);
                    }
                }
                _ => {}
            }
        }
        13 => match action {
            "menu_up" => ui.set_controller_detail_index(
                move_model_selection(ui.get_controller_detail_index(), 2, -1)
            ),
            "menu_down" => ui.set_controller_detail_index(
                move_model_selection(ui.get_controller_detail_index(), 2, 1)
            ),
            "menu_confirm" => {
                if ui.get_controller_detail_index() == 0 {
                    ui.set_controller_mapping_index(0);
                    ui.set_controller_mapping_scroll_offset(0);
                    navigate_settings_view(ui, 15);
                } else {
                    ui.set_controller_tester_return_view(13);
                    navigate_settings_view(ui, 14);
                }
            }
            _ => {}
        },
        14 => {}
        15 => {
            let count = ui.get_controller_mapping().row_count() as i32;
            if ui.get_controller_remap_waiting() || count == 0 {
                return;
            }
            let Some(entry) = selected_mapping_entry(ui) else {
                return;
            };
            let stick = if entry.control.as_str() == "left_deadzone" { "left" } else { "right" };
            match action {
                "menu_up" | "menu_down" => {
                    let step = if action == "menu_up" { -1 } else { 1 };
                    ui.set_controller_mapping_reset_armed(false);
                    ui.set_controller_mapping_index(
                        move_model_selection(ui.get_controller_mapping_index(), count, step)
                    );
                    update_controller_mapping_scroll(ui);
                }
                "menu_left" if entry.kind == 2 => set_controller_deadzone(ui, stick, -5),
                "menu_right" | "menu_confirm" if entry.kind == 2 => {
                    set_controller_deadzone(ui, stick, 5)
                }
                "menu_confirm" if entry.kind == 3 => {
                    if ui.get_controller_mapping_reset_armed() {
                        reset_controller_mapping(ui);
                    } else {
                        ui.set_controller_mapping_reset_armed(true);
                    }
                }
                "menu_confirm" => begin_controller_remap(ui),
                _ => {}
            }
        }
        16 => match action {
            "menu_up" => ui.set_rumble_index(
                move_model_selection(ui.get_rumble_index(), 2, -1)
            ),
            "menu_down" => ui.set_rumble_index(
                move_model_selection(ui.get_rumble_index(), 2, 1)
            ),
            "menu_left" | "menu_right" | "menu_confirm" if ui.get_rumble_index() == 0 => {
                rumble_set_enabled(ui, !ui.get_rumble_enabled());
            }
            "menu_confirm" if ui.get_rumble_index() == 1 => rumble_test(),
            _ => {}
        },
        17 => {
            if ui.get_lighting_preview_open() {
                match action {
                    "menu_left" => {
                        ui.set_lighting_preview_index(
                            (ui.get_lighting_preview_index() + 4).rem_euclid(5)
                        );
                        preview_lighting_effect(ui);
                    }
                    "menu_right" | "menu_confirm" => {
                        ui.set_lighting_preview_index(
                            (ui.get_lighting_preview_index() + 1).rem_euclid(5)
                        );
                        preview_lighting_effect(ui);
                    }
                    _ => {}
                }
            } else {
                let fixed_color = matches!(
                    ui.get_lighting_mode().as_str(),
                    "static" | "breathe" | "pulse" | "chase" | "wave"
                );
                match action {
                    "menu_up" => ui.set_lighting_index(
                        move_model_selection(ui.get_lighting_index(), 5, -1)
                    ),
                    "menu_down" => ui.set_lighting_index(
                        move_model_selection(ui.get_lighting_index(), 5, 1)
                    ),
                    "menu_confirm" if ui.get_lighting_index() == 0 => {
                        open_lighting_mode_dropdown(ui);
                    }
                    "menu_left" if ui.get_lighting_index() == 1 => {
                        let value = (ui.get_lighting_brightness() - 10).max(0);
                        lighting_call(ui, "SetBrightness", LightingArgs::Brightness(value));
                    }
                    "menu_right" | "menu_confirm" if ui.get_lighting_index() == 1 => {
                        let value = (ui.get_lighting_brightness() + 10).min(100);
                        lighting_call(ui, "SetBrightness", LightingArgs::Brightness(value));
                    }
                    "menu_confirm" if ui.get_lighting_index() == 2 && fixed_color => {
                        open_lighting_color_dropdown(ui);
                    }
                    "menu_left" | "menu_right" | "menu_confirm" if ui.get_lighting_index() == 3 => {
                        lighting_call(
                            ui,
                            "SetSystemEffects",
                            LightingArgs::Toggle(!ui.get_lighting_system_effects()),
                        );
                    }
                    "menu_confirm" if ui.get_lighting_index() == 4 => {
                        ui.set_lighting_preview_index(0);
                        ui.set_lighting_preview_open(true);
                        preview_lighting_effect(ui);
                    }
                    _ => {}
                }
            }
        },
        23 => match action {"menu_up"=>{ui.set_general_index(move_model_selection(ui.get_general_index(),12,-1));update_general_scroll(ui);},"menu_down"=>{ui.set_general_index(move_model_selection(ui.get_general_index(),12,1));update_general_scroll(ui);},_ if (6..=9).contains(&ui.get_general_index())=>date_time_row_action(ui,ui.get_general_index()-6,action),"menu_confirm"=>match ui.get_general_index(){0=>open_active_user(ui),1=>open_language_dropdown(ui),3=>{ui.set_user_list_index(0);ui.set_user_list_scroll(0);navigate_settings_view(ui,25);},4 if ui.get_user_count()>1=>open_startup(ui),5 if ui.get_user_count()>1&&ui.get_user_login_mode().as_str()=="default"=>open_default_user(ui),10 if ui.get_automatic_time()=>{ui.set_regional_notice(tr(ui,323,"Syncing…").into());let w=ui.as_weak();thread::spawn(move||{let ok=regional_sync_command().map(|r|r.starts_with("OK")).unwrap_or(false);let _=slint::invoke_from_event_loop(move||if let Some(ui)=w.upgrade(){ui.set_regional_notice(if ok{tr(&ui,299,"Time synchronized").into()}else{tr(&ui,300,"Time synchronization failed").into()});});});},11=>open_keyboard_choice(ui),_=>{}},_=>{}},
        24 => match action {"menu_up"=>ui.set_profile_index(move_model_selection(ui.get_profile_index(),2,-1)),"menu_down"=>ui.set_profile_index(move_model_selection(ui.get_profile_index(),2,1)),"menu_confirm" if ui.get_profile_index()==0=>open_system_keyboard(ui,&tr(ui,285,"Username"),8,24,"text",ui.get_profile_edit_name().as_str()),"menu_confirm" if ui.get_profile_index()==1=>open_avatar_picker(ui),_=>{}},
        25 => {let n=ui.get_user_count()+1;match action{"menu_up"=>{ui.set_user_list_index(move_model_selection(ui.get_user_list_index(),n,-1));update_user_scroll(ui);},"menu_down"=>{ui.set_user_list_index(move_model_selection(ui.get_user_list_index(),n,1));update_user_scroll(ui);},"menu_confirm" if ui.get_user_list_index()==ui.get_user_count()=>open_system_keyboard(ui,&tr(ui,296,"Enter username"),9,25,"text",""),"menu_confirm"=>open_selected_user(ui),_=>{}}},
        26 => match action {"menu_up"=>{ui.set_profile_index(move_model_selection(ui.get_profile_index(),3,-1));ui.set_user_delete_confirm(false);},"menu_down"=>{ui.set_profile_index(move_model_selection(ui.get_profile_index(),3,1));ui.set_user_delete_confirm(false);},"menu_confirm" if ui.get_profile_index()==0=>open_system_keyboard(ui,&tr(ui,285,"Username"),8,26,"text",ui.get_profile_edit_name().as_str()),"menu_confirm" if ui.get_profile_index()==1=>open_avatar_picker(ui),"menu_confirm" if ui.get_profile_index()==2&&ui.get_profile_edit_user_id()!=ui.get_active_user_id()&&ui.get_user_count()>1=>{if ui.get_user_delete_confirm(){let id=ui.get_profile_edit_user_id().to_string();ui.set_user_delete_confirm(false);navigate_settings_view(ui,users_page_view(ui));thread::spawn(move||{let _=users_command(&format!("DELETE	{}	CONFIRM",id));});}else{ui.set_user_delete_confirm(true);}},_=>{}},
        19 => {
            match action {
                "menu_up" => {
                    ui.set_system_index(move_model_selection(ui.get_system_index(), 8, -1));
                    update_system_scroll(ui);
                }
                "menu_down" => {
                    ui.set_system_index(move_model_selection(ui.get_system_index(), 8, 1));
                    update_system_scroll(ui);
                }
                "menu_confirm" => match ui.get_system_index() {
                    0 => open_system_profile_dropdown(ui),
                    1 => open_auto_battery_dropdown(ui),
                    2 => open_screensaver_timeout_dropdown(ui),
                    3 => open_sleep_timeout_dropdown(ui),
                    4 => open_poweroff_timeout_dropdown(ui),
                    5 => {
                        ui.set_system_storage_index(0);
                        ui.set_system_storage_scroll_offset(0);
                        ui.set_system_storage_confirm(false);
                        navigate_settings_view(ui, 20);
                        refresh_system(ui);
                    },
                    6 => {
                        ui.set_system_info_index(0);
                        ui.set_system_info_scroll_offset(0);
                        navigate_settings_view(ui, 21);
                    },
                    7 => {
                        ui.set_system_reset_confirm(false);
                        navigate_settings_view(ui, 22);
                    }
                    _ => {}
                },
                _ => {}
            }
        }
        20 => {
            let job_running = ui.get_system_storage_job_state().as_str() == "running";
            match action {
                "menu_up" => {
                    ui.set_system_storage_index(
                        move_model_selection(ui.get_system_storage_index(), 7, -1)
                    );
                    ui.set_system_storage_confirm(false);
                    update_system_storage_scroll(ui);
                }
                "menu_down" => {
                    ui.set_system_storage_index(
                        move_model_selection(ui.get_system_storage_index(), 7, 1)
                    );
                    ui.set_system_storage_confirm(false);
                    update_system_storage_scroll(ui);
                }
                /* Rows 0, 1, 2 and 6 are deliberately read-only: Confirm does nothing. */
                "menu_confirm" if ui.get_system_storage_index() == 3 && !job_running => {
                    open_storage_backup_policy_dropdown(ui);
                }
                "menu_confirm" if ui.get_system_storage_index() == 4 && !job_running => {
                    let tf2 = ui.get_system_tf2_state().to_string();
                    let active = ui.get_system_storage_active().to_string();
                    if tf2 == "FOREIGN" {
                        let cid = ui.get_system_tf2_cid().to_string();
                        if !cid.is_empty() {
                            if ui.get_system_storage_confirm() {
                                ui.set_system_storage_confirm(false);
                                system_set(ui, format!("START STORAGE ADOPT {}", cid));
                            } else {
                                ui.set_system_storage_confirm(true);
                            }
                        }
                    } else if tf2 == "OWNED" && active == "TF2" {
                        if ui.get_system_storage_confirm() {
                            ui.set_system_storage_confirm(false);
                            system_set(ui, "START STORAGE MOVE_BACK".to_owned());
                        } else {
                            ui.set_system_storage_confirm(true);
                        }
                    } else if tf2 == "OWNED" {
                        ui.set_system_storage_confirm(false);
                        system_set(ui, "SET STORAGE MODE AUTO".to_owned());
                    }
                }
                "menu_confirm" if ui.get_system_storage_index() == 5 && !job_running => {
                    if ui.get_system_tf2_state().as_str() == "OWNED"
                        && ui.get_system_storage_active().as_str() == "TF2"
                    {
                        system_set(ui, "START STORAGE BACKUP".to_owned());
                    }
                }
                _ => {}
            }
        }
        21 => match action {
            "menu_up" => {
                ui.set_system_info_index(
                    move_model_selection(ui.get_system_info_index(), 10, -1)
                );
                update_system_info_scroll(ui);
            }
            "menu_down" => {
                ui.set_system_info_index(
                    move_model_selection(ui.get_system_info_index(), 10, 1)
                );
                update_system_info_scroll(ui);
            }
            /* System Information is read-only by design. */
            "menu_confirm" => {}
            _ => {}
        },
        22 => {
            if action == "menu_confirm" {
                if ui.get_system_reset_confirm() {
                    ui.set_system_reset_confirm(false);
                    system_set(ui, "RESET SYSTEM SETTINGS".to_owned());
                    navigate_settings_view(ui, 19);
                } else {
                    ui.set_system_reset_confirm(true);
                }
            }
        }
        18 => {
            let count = if ui.get_display_color_temperature_available() { 10 } else { 9 };
            match action {
                "menu_up" => {
                    ui.set_display_audio_index(
                        move_model_selection(ui.get_display_audio_index(), count, -1)
                    );
                    update_display_audio_scroll(ui, count);
                }
                "menu_down" => {
                    ui.set_display_audio_index(
                        move_model_selection(ui.get_display_audio_index(), count, 1)
                    );
                    update_display_audio_scroll(ui, count);
                }
                "menu_left" if ui.get_display_audio_index() == 0 => adjust_display_brightness(ui, -1),
                "menu_right" if ui.get_display_audio_index() == 0 => adjust_display_brightness(ui, 1),
                "menu_confirm" if ui.get_display_audio_index() == 1 => open_audio_output_dropdown(ui),
                "menu_left" if ui.get_display_audio_index() == 2 => audio_adjust_volume(ui, -1),
                "menu_right" if ui.get_display_audio_index() == 2 => audio_adjust_volume(ui, 1),
                "menu_left" if ui.get_display_audio_index() == 3 => adjust_applications_volume(ui, -1),
                "menu_right" if ui.get_display_audio_index() == 3 => adjust_applications_volume(ui, 1),
                "menu_left" if ui.get_display_audio_index() == 4 => adjust_system_sounds_volume(ui, -1),
                "menu_right" if ui.get_display_audio_index() == 4 => adjust_system_sounds_volume(ui, 1),
                "menu_left" | "menu_right" | "menu_confirm" if ui.get_display_audio_index() == 5 => {
                    set_navigation_sounds_enabled(ui, !ui.get_navigation_sounds_enabled());
                }
                "menu_left" | "menu_right" | "menu_confirm" if ui.get_display_audio_index() == 6 => {
                    set_power_sounds_enabled(ui, !ui.get_power_sounds_enabled());
                }
                "menu_left" if ui.get_display_audio_index() == 7 => adjust_home_music_volume(ui, -1),
                "menu_right" if ui.get_display_audio_index() == 7 => adjust_home_music_volume(ui, 1),
                "menu_confirm" if ui.get_display_audio_index() == 8 => audio_toggle_test_tone(ui),
                "menu_left" if ui.get_display_audio_index() == 9
                    && ui.get_display_color_temperature_available() => adjust_display_color_temperature(ui, -250),
                "menu_right" if ui.get_display_audio_index() == 9
                    && ui.get_display_color_temperature_available() => adjust_display_color_temperature(ui, 250),
                _ => {}
            }
        },
        6 => match action {
            "menu_left" => move_keyboard_horizontal(ui, -1),
            "menu_right" => move_keyboard_horizontal(ui, 1),
            "menu_up" => move_keyboard_vertical(ui, -1),
            "menu_down" => move_keyboard_vertical(ui, 1),
            "menu_confirm" => handle_system_keyboard_confirm(ui),
            _ => {}
        },
        _ => {}
    }
}

fn close_power_menu(ui: &HomeWindow, power_active: &Arc<AtomicBool>)
{
    power_active.store(false, Ordering::SeqCst);
    ui.set_power_menu_open(false);
    ui.set_power_menu_confirm(false);
}

fn invoke_power_action(ui: &HomeWindow, action: &'static str, power_active: &Arc<AtomicBool>)
{
    close_power_menu(ui, power_active);
    let command = match action {
        "sleep" => "ACTION SLEEP",
        "restart" => "ACTION RESTART",
        "poweroff" => "ACTION POWEROFF",
        _ => return,
    }.to_owned();
    thread::spawn(move || {
        if let Err(error) = system_command(&command) {
            eprintln!("home: lifecycle command {:?} failed={error}", command);
        }
    });
}

fn handle_power_action(ui: &HomeWindow, action: &str, power_active: &Arc<AtomicBool>)
{
    play_ui_sound(action);
    let current = ui.get_power_menu_index();
    match action {
        "menu_back" => close_power_menu(ui, power_active),
        "menu_up" => {
            ui.set_power_menu_index(0);
            ui.set_power_menu_confirm(false);
        }
        "menu_down" if current == 0 => {
            ui.set_power_menu_index(1);
            ui.set_power_menu_confirm(false);
        }
        "menu_left" if current > 0 => {
            ui.set_power_menu_index(1);
            ui.set_power_menu_confirm(false);
        }
        "menu_right" if current > 0 => {
            ui.set_power_menu_index(2);
            ui.set_power_menu_confirm(false);
        }
        "menu_confirm" if current == 0 => invoke_power_action(ui, "sleep", power_active),
        "menu_confirm" if current == 1 || current == 2 => {
            if ui.get_power_menu_confirm() {
                invoke_power_action(
                    ui,
                    if current == 1 { "restart" } else { "poweroff" },
                    power_active,
                );
            } else {
                ui.set_power_menu_confirm(true);
            }
        }
        _ => {}
    }
}

fn start_input_listener(
    ui: &HomeWindow,
    settings_active: Arc<AtomicBool>,
    power_active: Arc<AtomicBool>,
    user_picker_active: Arc<AtomicBool>,
    screensaver_gate: Arc<(Mutex<bool>, Condvar)>,
) {
    let weak = ui.as_weak();

    thread::spawn(move || loop {
        match UnixStream::connect(INPUT_SOCKET) {
            Ok(mut stream) => {
                if let Err(error) = stream.write_all(b"SUBSCRIBE HOME\n") {
                    eprintln!("home: input subscribe failed={}", error);
                    thread::sleep(Duration::from_millis(250));
                    continue;
                }

                set_ui_capture(true);

                let mut reader = BufReader::new(stream);
                let mut line = String::new();

                loop {
                    line.clear();
                    match reader.read_line(&mut line) {
                        Ok(0) => break,
                        Ok(_) => {
                            let fields: Vec<&str> = line.split_whitespace().collect();
                            if fields.len() != 4 ||
                                fields[0] != "EVENT" ||
                                fields[1] != "1" ||
                                fields[3] != "pressed"
                            {
                                continue;
                            }

                            {
                                let (lock, _) = &*screensaver_gate;
                                let mut active = lock.lock().unwrap();
                                if *active {
                                    *active = false;
                                    let _ = system_command("ACTIVITY");
                                    let weak = weak.clone();
                                    let _ = slint::invoke_from_event_loop(move || {
                                        if let Some(ui) = weak.upgrade() {
                                            ui.set_screensaver_active(false);
                                        }
                                    });
                                    continue;
                                }
                            }

                            if user_picker_active.load(Ordering::SeqCst) && matches!(fields[2],"menu_left"|"menu_right"|"menu_up"|"menu_down"|"menu_confirm"|"menu_back"){let a=fields[2].to_owned();let w=weak.clone();let p=user_picker_active.clone();let _=slint::invoke_from_event_loop(move||if let Some(ui)=w.upgrade(){handle_user_picker(&ui,&a,&p);});continue;}
                            if matches!(fields[2], "power" | "power_menu" | "power_button" | "sleep") {
                                let open = !power_active.swap(true, Ordering::SeqCst);
                                let weak = weak.clone();
                                let power_active = power_active.clone();
                                let _ = slint::invoke_from_event_loop(move || {
                                    if let Some(ui) = weak.upgrade() {
                                        if open {
                                            ui.set_power_menu_index(0);
                                            ui.set_power_menu_confirm(false);
                                            ui.set_power_menu_open(true);
                                        } else {
                                            close_power_menu(&ui, &power_active);
                                        }
                                    }
                                });
                                continue;
                            }

                            if power_active.load(Ordering::SeqCst) && matches!(
                                fields[2],
                                "menu_up" | "menu_down" | "menu_left" | "menu_right" |
                                "menu_confirm" | "menu_back"
                            ) {
                                let action = fields[2].to_owned();
                                let weak = weak.clone();
                                let power_active = power_active.clone();
                                let _ = slint::invoke_from_event_loop(move || {
                                    if let Some(ui) = weak.upgrade() {
                                        handle_power_action(&ui, &action, &power_active);
                                    }
                                });
                                continue;
                            }

                            if OOB_ACTIVE.load(Ordering::SeqCst) {
                                if matches!(
                                    fields[2],
                                    "menu_up" | "menu_down" | "menu_left" | "menu_right" |
                                    "menu_confirm" | "menu_back"
                                ) {
                                    let action = fields[2].to_owned();
                                    let weak = weak.clone();
                                    let settings_active = settings_active.clone();
                                    let _ = slint::invoke_from_event_loop(move || {
                                        if let Some(ui) = weak.upgrade() {
                                            handle_oob_action(&ui, &action, &settings_active);
                                        }
                                    });
                                }
                                /* Start (Settings) is not available during setup. */
                                continue;
                            }

                            if fields[2] == "settings" {
                                if settings_active.swap(true, Ordering::SeqCst) {
                                    continue;
                                }
                                play_ui_sound("settings");
                                set_ui_capture(true);
                                let weak = weak.clone();
                                let _ = slint::invoke_from_event_loop(move || {
                                    if let Some(ui) = weak.upgrade() {
                                        ui.set_settings_selected_index(0);
                                        navigate_settings_view(&ui, 0);
                                        ui.set_settings_open(true);
                                        write_ui_context("settings");
                                        wifi_notice(&ui, "");
                                    }
                                });
                                continue;
                            }

                            if settings_active.load(Ordering::SeqCst) && matches!(
                                fields[2],
                                "menu_up" | "menu_down" | "menu_left" | "menu_right" |
                                "menu_confirm" | "menu_back"
                            ) {
                                let action = fields[2].to_owned();
                                let weak = weak.clone();
                                let settings_active = settings_active.clone();
                                let _ = slint::invoke_from_event_loop(move || {
                                    if let Some(ui) = weak.upgrade() {
                                        handle_settings_action(&ui, &action, &settings_active);
                                    }
                                });
                                continue;
                            }

                            /* Home carousels / library grid. */
                            if !settings_active.load(Ordering::SeqCst) &&
                                !user_picker_active.load(Ordering::SeqCst) &&
                                !power_active.load(Ordering::SeqCst) &&
                                matches!(
                                    fields[2],
                                    "menu_up" | "menu_down" | "menu_left" | "menu_right" |
                                    "menu_confirm" | "menu_back" | "face_north"
                                )
                            {
                                let action = fields[2].to_owned();
                                let weak = weak.clone();
                                let _ = slint::invoke_from_event_loop(move || {
                                    if let Some(ui) = weak.upgrade() {
                                        library::handle_home_action(&ui, &action);
                                    }
                                });
                            }
                        }
                        Err(error) => {
                            eprintln!("home: input read failed={}", error);
                            break;
                        }
                    }
                }
            }
            Err(error) => {
                eprintln!("home: input connect failed={}", error);
            }
        }

        thread::sleep(Duration::from_millis(250));
    });
}

/* Date & Time rows shared by General (rows 6-9) and the OOB (rows 0-3):
 * 0 Timezone, 1 Automatic Time, 2 Date, 3 Time. */
fn date_time_row_action(ui:&HomeWindow,row:i32,action:&str){
    let manual=!ui.get_automatic_time();
    let step=if action=="menu_left"{-1}else{1};
    match (row,action){
        (0,"menu_confirm")=>open_timezone_region(ui),
        (1,"menu_confirm")=>{
            let v=if ui.get_automatic_time(){"0"}else{"1"};
            thread::spawn(move||{let _=regional_command(&format!("SET_AUTOMATIC_TIME\t{}",v));});
        }
        (2,"menu_left"|"menu_right") if manual=>{
            thread::spawn(move||{let _=regional_command(&format!("ADJUST_LOCAL_DAYS\t{}",step));});
        }
        (3,"menu_left"|"menu_right") if manual=>{
            thread::spawn(move||{let _=regional_command(&format!("ADJUST_LOCAL_MINUTES\t{}",step));});
        }
        _=>{}
    }
}

/* OOB Users page rows: users, Add User, [Startup, Boot User], Continue
 * (same order as home.slint). */
fn oob_users_rows(ui:&HomeWindow)->i32{
    let n=ui.get_user_count();
    n+2+if n>1{2}else{0}
}
fn update_oob_users_scroll(ui:&HomeWindow){
    ui.set_oob_scroll(guarded_scroll_offset(
        ui.get_oob_index(),oob_users_rows(ui),ui.get_settings_list_visible_rows(),ui.get_oob_scroll()
    ));
}

fn enter_oob(ui:&HomeWindow){
    OOB_ACTIVE.store(true,Ordering::SeqCst);
    ui.set_oob_active(true);
    ui.set_oob_step(0);
    ui.set_oob_index(0);
    ui.set_oob_scroll(0);
    ui.set_oob_notice("".into());
    ui.set_settings_view(30);
    ui.set_settings_open(true);
    write_ui_context("oob");
    thread::spawn(||set_ui_capture(true));
}

fn leave_oob(ui:&HomeWindow){
    OOB_ACTIVE.store(false,Ordering::SeqCst);
    ui.set_avatar_picker_open(false);
    navigate_settings_view(ui,0);
    ui.set_settings_selected_index(0);
    ui.set_settings_open(false);
    ui.set_oob_active(false);
    write_ui_context("home");
}

fn oob_go(ui:&HomeWindow,step:i32){
    ui.set_oob_step(step);
    ui.set_oob_index(0);
    ui.set_oob_scroll(0);
    ui.set_oob_notice("".into());
    match step{
        1=>navigate_settings_view(ui,30),
        2=>{wifi_notice(ui,"");navigate_settings_view(ui,31);}
        3=>navigate_settings_view(ui,32),
        _=>{}
    }
}

/* Ready → usersd commits setup and starts the session; its snapshot then
 * closes the OOB (and opens the user picker when the login mode asks). */
fn oob_finish(ui:&HomeWindow){
    let w=ui.as_weak();
    thread::spawn(move||{
        let reply=users_command("COMPLETE_SETUP").unwrap_or_else(|e|format!("ERR {e}"));
        if reply.starts_with("OK"){return;}
        eprintln!("home: complete setup failed: {}",reply.trim());
        let no_users=reply.starts_with("ERR no users");
        let _=slint::invoke_from_event_loop(move||if let Some(ui)=w.upgrade(){
            if no_users{
                oob_go(&ui,3);
                ui.set_oob_notice(tr(&ui,298,"At least one user is required").into());
            }
        });
    });
}

fn handle_oob_action(ui:&HomeWindow,action:&str,settings_active:&Arc<AtomicBool>){
    /* Dropdowns and the avatar picker behave exactly as in Settings. */
    if ui.get_avatar_picker_open()||ui.get_settings_choice_open(){
        handle_settings_action(ui,action,settings_active);
        return;
    }
    let step=ui.get_oob_step();
    if step==0||step==4{
        play_ui_sound(action);
        match action{
            "menu_confirm" if step==0=>oob_go(ui,1),
            "menu_confirm"=>oob_finish(ui),
            "menu_back" if step==4=>oob_go(ui,3),
            _=>{}
        }
        return;
    }
    let view=ui.get_settings_view();
    let index=ui.get_oob_index();
    match (view,action){
        (30|31|32,_)=>play_ui_sound(action),
        /* Reused Settings pages return to their OOB step. */
        (2,"menu_back")=>{play_ui_sound(action);navigate_settings_view(ui,31);return;}
        (26,"menu_back")=>{play_ui_sound(action);ui.set_user_delete_confirm(false);navigate_settings_view(ui,32);return;}
        _=>{handle_settings_action(ui,action,settings_active);return;}
    }
    match view{
        30=>match action{
            "menu_up"=>ui.set_oob_index(move_model_selection(index,5,-1)),
            "menu_down"=>ui.set_oob_index(move_model_selection(index,5,1)),
            "menu_back"=>oob_go(ui,0),
            "menu_confirm" if index==4=>oob_go(ui,2),
            _=>date_time_row_action(ui,index,action),
        },
        31=>match action{
            "menu_up"=>ui.set_oob_index(move_model_selection(index,3,-1)),
            "menu_down"=>ui.set_oob_index(move_model_selection(index,3,1)),
            "menu_back"=>oob_go(ui,1),
            "menu_confirm"=>match index{
                0=>wifi_set_enabled(ui,!ui.get_wifi_enabled()),
                1 if ui.get_wifi_enabled()=>navigate_settings_view(ui,2),
                2=>oob_go(ui,3),
                _=>{}
            },
            _=>{}
        },
        _=>{
            let n=ui.get_user_count();
            let total=oob_users_rows(ui);
            match action{
                "menu_up"|"menu_down"=>{
                    ui.set_oob_index(move_model_selection(index,total,if action=="menu_up"{-1}else{1}));
                    update_oob_users_scroll(ui);
                }
                "menu_back"=>oob_go(ui,2),
                "menu_confirm" if index<n=>{
                    if let Some(u)=ui.get_users().row_data(index as usize){open_user(ui,u,26);}
                }
                "menu_confirm" if index==n=>{
                    ui.set_oob_notice("".into());
                    open_system_keyboard(ui,&tr(ui,296,"Enter username"),9,32,"text","");
                }
                "menu_confirm" if n>1&&index==n+1=>open_startup(ui),
                "menu_confirm" if n>1&&index==n+2&&ui.get_user_login_mode().as_str()=="default"=>open_default_user(ui),
                "menu_confirm" if index==total-1=>{
                    if n>0{oob_go(ui,4);}
                    else{ui.set_oob_notice(tr(ui,298,"At least one user is required").into());}
                }
                _=>{}
            }
        }
    }
}

fn handle_user_picker(ui:&HomeWindow,a:&str,p:&Arc<AtomicBool>){
    let n=ui.get_users().row_count() as i32;
    match a{
        "menu_left"|"menu_up" if n>0=>{
            ui.set_user_picker_index(move_model_selection(ui.get_user_picker_index(),n,-1));
        },
        "menu_right"|"menu_down" if n>0=>{
            ui.set_user_picker_index(move_model_selection(ui.get_user_picker_index(),n,1));
        },
        "menu_confirm" if n>0=>{
            if let Some(u)=ui.get_users().row_data(ui.get_user_picker_index().max(0) as usize){
                let id=u.id.to_string();
                let w=ui.as_weak();
                let picker=p.clone();
                thread::spawn(move||{
                    /* HomeWindow does not expose Home Music playback state as a
                     * Slint property. Query Product Audio directly so Switch User
                     * follows the service source of truth instead of UI state. */
                    let music_was_playing=audio_socket_command("STATUS")
                        .map(|r|r.lines().any(|line|line.trim()=="home_music.playing=1"))
                        .unwrap_or(false);
                    let ok=users_command(&format!("ACTIVATE\t{}",id))
                        .map(|r|r.starts_with("OK"))
                        .unwrap_or(false);
                    let _=slint::invoke_from_event_loop(move||if let Some(ui)=w.upgrade(){
                        if ok {
                            ui.set_user_picker_open(false);
                            picker.store(false,Ordering::SeqCst);
                            /* Product Audio resolves /run/nuubos/userdata on each
                             * new track. Restart Home Music so the newly active
                             * user's library becomes authoritative immediately. */
                            if music_was_playing {
                                thread::spawn(move||{
                                    let _=audio_socket_command("MUSIC STOP");
                                    let _=audio_socket_command("MUSIC START");
                                });
                            }
                        } else {
                            eprintln!("home: user activation failed for {}",id);
                        }
                    });
                });
            }
        },
        "menu_back" if !ui.get_user_picker_forced()=>{
            ui.set_user_picker_open(false);
            p.store(false,Ordering::SeqCst);
            thread::spawn(move||{let _=users_command("CANCEL_SWITCH");});
        },
        _=>{}
    }
}

fn mark_ready() {
    let _ = fs::create_dir_all("/run/nuubos");
    let _ = fs::write(UI_READY, b"ready\n");
}

fn clear_ready() {
    let _ = fs::remove_file(UI_READY);
}

/// Render the lifecycle splash for `mode` at the window's physical size.
fn prepare_splash(ui: &HomeWindow, mode: &str) {
    let size = ui.window().size();
    let scale = ui.window().scale_factor().max(0.1);
    let (split, line_fraction, line_px) = splash::geometry(size.width, size.height);
    ui.set_splash_image(splash::render(mode, size.width, size.height));
    ui.set_splash_split(split);
    ui.set_splash_line_fraction(line_fraction);
    ui.set_splash_line_height(line_px as f32 / scale);
}

fn start_shutdown_transition(ui: &HomeWindow, mode: String) {
    prepare_splash(ui, &mode);
    ui.set_transition_mode(mode.into());
    ui.set_curtain_progress(0.0);

    let weak = ui.as_weak();
    Timer::single_shot(
        Duration::from_millis(CURTAIN_STAGE_MS + STAGE_GAP_MS),
        move || {
            if let Some(ui) = weak.upgrade() {
                ui.set_line_progress(0.0);
            }
        },
    );
}

fn start_boot_transition(ui: &HomeWindow) {
    prepare_splash(ui, "boot");
    ui.set_transition_mode("boot".into());
    ui.set_line_progress(1.0);

    let weak = ui.as_weak();
    Timer::single_shot(
        Duration::from_millis(LINE_STAGE_MS + STAGE_GAP_MS),
        move || {
            if let Some(ui) = weak.upgrade() {
                ui.set_curtain_progress(1.0);
            }
        },
    );
}

fn start_screensaver_animator(
    ui: &HomeWindow,
    gate: Arc<(Mutex<bool>, Condvar)>,
) {
    let weak = ui.as_weak();

    thread::spawn(move || {
        let mut x: i32 = 110;
        let mut y: i32 = 170;
        let mut dx: i32 = 7;
        let mut dy: i32 = 5;

        loop {
            let (lock, cv) = &*gate;
            let mut active = lock.lock().unwrap();
            while !*active {
                active = cv.wait(active).unwrap();
            }
            drop(active);

            x += dx;
            y += dy;
            if x <= 0 { x = 0; dx = dx.abs(); }
            else if x >= 1000 { x = 1000; dx = -dx.abs(); }
            if y <= 0 { y = 0; dy = dy.abs(); }
            else if y >= 1000 { y = 1000; dy = -dy.abs(); }

            let weak = weak.clone();
            let px = x;
            let py = y;
            let _ = slint::invoke_from_event_loop(move || {
                if let Some(ui) = weak.upgrade() {
                    ui.set_screensaver_x_permille(px);
                    ui.set_screensaver_y_permille(py);
                }
            });

            thread::sleep(Duration::from_millis(80));
        }
    });
}

fn start_lifecycle_listener(ui: &HomeWindow) {
    let weak = ui.as_weak();

    thread::spawn(move || loop {
        match File::open(UI_CONTROL) {
            Ok(file) => {
                let reader = BufReader::new(file);

                for line in reader.lines() {
                    let Ok(line) = line else {
                        break;
                    };

                    let mode = line.trim().to_owned();
                    if mode != "reboot" && mode != "poweroff" {
                        continue;
                    }

                    let weak = weak.clone();
                    let _ = slint::invoke_from_event_loop(move || {
                        if let Some(ui) = weak.upgrade() {
                            start_shutdown_transition(&ui, mode);
                        }
                    });
                }
            }
            Err(_) => thread::sleep(Duration::from_millis(50)),
        }
    });
}

/* GPU rendering (FemtoVG on OpenGL ES / Panfrost): frames reach labwc as
 * dmabufs, so the compositor neither copies them nor composites a
 * fullscreen Home (direct scanout). NUUBOS_UI_RENDERER=software, or a
 * failure to get an OpenGL ES backend, falls back to the CPU renderer. */
fn select_renderer() {
    let software = std::env::var("NUUBOS_UI_RENDERER").map(|v| v == "software").unwrap_or(false);
    if !software {
        match slint::BackendSelector::new()
            .backend_name("winit".into())
            .renderer_name("femtovg".into())
            .require_opengl_es()
            .select()
        {
            Ok(()) => return,
            Err(e) => eprintln!("nuubui-home: GPU renderer unavailable ({e}), using software"),
        }
    }
    if let Err(e) = slint::BackendSelector::new()
        .backend_name("winit".into())
        .renderer_name("software".into())
        .select()
    {
        eprintln!("nuubui-home: software renderer selection failed: {e}");
    }
}

fn main() -> Result<(), slint::PlatformError> {
    select_renderer();
    let ui = HomeWindow::new()?;
    ui.window().set_fullscreen(true);

    apply_language(&ui, "en");
    let user_picker_active=Arc::new(AtomicBool::new(false));
    refresh_users(&ui,&user_picker_active); refresh_regional(&ui);
    start_users_listener(&ui,user_picker_active.clone()); start_regional_listener(&ui);
    start_localization_listener(&ui); start_status_listener(&ui);
    start_wifi_product_listener(&ui);
    start_bluetooth_product_listener(&ui);
    start_controllers_product_listener(&ui);
    start_rumble_listener(&ui);
    start_lighting_listener(&ui);
    start_audio_product_listener(&ui);
    library::start_cover_loader(&ui);
    library::start_library_listener(&ui);
    let system_info_gate = Arc::new((Mutex::new(false), Condvar::new()));
    let _ = SYSTEM_INFO_GATE.set(system_info_gate.clone());
    start_system_info_live(&ui, system_info_gate);
    let screensaver_gate = Arc::new((Mutex::new(false), Condvar::new()));
    start_system_listener(&ui, screensaver_gate.clone());
    start_screensaver_animator(&ui, screensaver_gate.clone());
    start_lifecycle_listener(&ui);
    refresh_hint_mapping(&ui);

    let splash_weak = ui.as_weak();
    ui.on_splash_size_changed(move || {
        if let Some(ui) = splash_weak.upgrade() {
            let mode = ui.get_transition_mode().to_string();
            prepare_splash(&ui, &mode);
        }
    });

    let keyboard_weak = ui.as_weak();
    ui.on_system_keyboard_accepted(move || {
        if let Some(ui) = keyboard_weak.upgrade() {
            finish_system_keyboard(&ui);
        }
    });

    let settings_active = Arc::new(AtomicBool::new(false));
    let power_active = Arc::new(AtomicBool::new(false));
    start_input_listener(
        &ui,
        settings_active.clone(),
        power_active.clone(),
        user_picker_active.clone(),
        screensaver_gate.clone(),
    );

    if !OOB_ACTIVE.load(Ordering::SeqCst) {
        write_ui_context("home");
    }
    let weak = ui.as_weak();
    Timer::single_shot(Duration::from_millis(220), move || {
        if let Some(ui) = weak.upgrade() {
            mark_ready();
            start_boot_transition(&ui);
        }
    });

    let result = ui.run();

    set_ui_capture(false);

    stop_home_music_session();
    clear_ready();
    result
}
