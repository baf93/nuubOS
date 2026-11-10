/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* Exercise the actual daemon policy, registry callbacks and public snapshot. */
#define main audiod_main
#include "../src/audiod.c"
#undef main
#include <assert.h>

static void expect_output(const char *mode, struct audio_outputs outputs,
			  const char *expected)
{
	assert(strcmp(audio_select_output(mode, &outputs), expected) == 0);
}

static void test_policy(void)
{
	struct audio_outputs out = {0};
	expect_output("auto", out, "speaker");
	expect_output("usb", out, "speaker");
	out.hdmi = true;
	expect_output("auto", out, "hdmi");
	expect_output("usb", out, "hdmi");
	out.headphones = true;
	expect_output("auto", out, "headphones");
	out.usb = true;
	expect_output("auto", out, "usb");
	expect_output("bluetooth", out, "usb");
	out.bluetooth = true;
	expect_output("auto", out, "bluetooth");
	expect_output("usb", out, "usb");
	expect_output("analog", out, "headphones");
	expect_output("hdmi", out, "hdmi");
	expect_output("speaker", out, "speaker");
	out.headphones = false;
	expect_output("analog", out, "speaker");
	out.usb = false;
	expect_output("usb", out, "hdmi");
	out.usb = true;
	expect_output("usb", out, "usb");
	out.cast = true;
	const char *modes[] = { "auto", "usb", "analog", "bluetooth", "hdmi", "speaker" };
	for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++)
		expect_output(modes[i], out, "cast");
}

static void add_node(struct sink_watch *watch, uint32_t id,
		     const char *name, const char *media_class)
{
	const struct spa_dict_item items[] = {
		{ PW_KEY_NODE_NAME, name }, { PW_KEY_MEDIA_CLASS, media_class },
	};
	const struct spa_dict props = SPA_DICT_INIT_ARRAY(items);
	sink_watch_global(watch, id, 0, PW_TYPE_INTERFACE_Node, 0, &props);
}

static void test_hotplug(void)
{
	struct audio_state state = {0};
	struct sink_watch watch = { .state = &state };
	add_node(&watch, 10, "nuubos_analog", "Audio/Sink");
	/* A capture-only microphone must not steal playback. */
	add_node(&watch, 20, "alsa_input.usb-Microphone-00.mono-fallback", "Audio/Source");
	sink_watch_usb_availability(&watch, &state);
	assert(!state.usb_available && state.usb_microphone_available);
	add_node(&watch, 30, "alsa_output.usb-Headset-00.analog-stereo", "Audio/Sink");
	add_node(&watch, 31, "alsa_input.usb-Headset-00.mono-fallback", "Audio/Source");
	add_node(&watch, 40, "alsa_output.usb-DAC-00.analog-stereo", "Audio/Sink");
	/* A sink monitor is not a microphone. */
	add_node(&watch, 41, "alsa_output.usb-DAC-00.monitor", "Audio/Source");
	sink_watch_usb_availability(&watch, &state);
	assert(state.usb_available && state.usb_microphone_available);
	strcpy(state.selected, "usb");
	strcpy(state.applied_route, "usb");
	strcpy(state.sink_id, "40");
	sink_watch_global_remove(&watch, 40);
	assert(state.applied_route[0] == '\0' && state.sink_id[0] == '\0');
	sink_watch_usb_availability(&watch, &state);
	assert(state.usb_available); /* The other headset is still connected. */
	sink_watch_global_remove(&watch, 30);
	sink_watch_usb_availability(&watch, &state);
	assert(!state.usb_available && state.usb_microphone_available);
	sink_watch_global_remove(&watch, 20);
	sink_watch_global_remove(&watch, 31);
	sink_watch_global_remove(&watch, 999);
	sink_watch_usb_availability(&watch, &state);
	assert(!state.usb_available && !state.usb_microphone_available);
	add_node(&watch, 50, "alsa_output.usb-Headset-00.analog-stereo", "Audio/Sink");
	sink_watch_usb_availability(&watch, &state);
	assert(state.usb_available);
	add_node(&watch, 60, "nuubos_cast", "Audio/Sink");
	assert(watch.cast_id == 60 && watch.cast_changed);
	sink_watch_global_remove(&watch, 60);
	assert(watch.cast_id == 0);
	sink_watch_close(&watch);
	assert(watch.nodes == NULL);
}

static void test_volume_and_contract(void)
{
	struct audio_state state = {0};
	char reply[2048];
	load_config(&state);
	assert(state.volume_usb == 50 && valid_output("usb") && volume_supported("usb"));
	assert(!volume_supported("hdmi"));
	set_volume_memory(&state, "usb", 23);
	strcpy(state.selected, "usb");
	strcpy(state.sink_id, "30");
	assert(selected_volume(&state) == 23);
	assert(apply_selected_master_volume(&state) == 0);
	assert(toggle_selected_mute(&state, NULL) == 0);
	assert(state.volume_usb == 0 && state.unmute_usb == 23);
	assert(toggle_selected_mute(&state, NULL) == 0);
	assert(state.volume_usb == 23 && state.volume_bluetooth == 50);
	assert(flush_volume_config(&state) == 0);
	assert(update_config("AUDIO_OUTPUT", "USB") == 0);
	struct audio_state restored = {0};
	load_config(&restored);
	assert(strcmp(restored.mode, "usb") == 0 && restored.volume_usb == 23);
	state.usb_available = true;
	state.usb_microphone_available = true;
	status_reply(&state, reply, sizeof(reply));
	assert(strstr(reply, "available.usb=1\n"));
	assert(strstr(reply, "capture.available.usb=1\n"));
	assert(strstr(reply, "volume.usb=23\n"));
	route_reply(&state, reply, sizeof(reply));
	assert(strcmp(reply, "default\n") == 0);
	state.usb_available = false;
	route_reply(&state, reply, sizeof(reply));
	assert(strcmp(reply, "unavailable\n") == 0);
	for (int version = 1; version <= 2; version++) {
		DBusMessage *message = dbus_message_new_signal(AUDIO_OBJECT_PATH,
			AUDIO_INTERFACE, version == 1 ? "StateChanged" : "StateChanged2");
		assert(message != NULL);
		append_state_args(message, &state, version == 2);
		assert(strcmp(dbus_message_get_signature(message),
			version == 1 ? "ssibbbbbiib" : "ssibbbbbiibbb") == 0);
		dbus_message_unref(message);
	}
}

static long helper_log_size(void)
{
	FILE *fp = fopen(getenv("PW_LOG"), "r");
	assert(fp != NULL);
	assert(fseek(fp, 0, SEEK_END) == 0);
	long size = ftell(fp);
	fclose(fp);
	return size;
}

static void test_registry_routing(void)
{
	struct audio_state state = {0};
	struct sink_watch watch = { .state = &state };
	load_config(&state);
	strcpy(state.mode, "auto");
	const char *analog = "\"10\":{\"node.name\":\"nuubos_analog\",\"media.class\":\"Audio/Sink\"}";
	char fixture[512];
	snprintf(fixture, sizeof(fixture), "{%s,"
		"\"30\":{\"node.name\":\"alsa_output.usb-Headset\",\"media.class\":\"Audio/Sink\"},"
		"\"40\":{\"node.name\":\"alsa_output.usb-DAC\",\"media.class\":\"Audio/Sink\"}}", analog);
	assert(setenv("PW_FIXTURE", fixture, 1) == 0);
	add_node(&watch, 10, "nuubos_analog", "Audio/Sink");
	add_node(&watch, 30, "alsa_output.usb-Headset", "Audio/Sink");
	add_node(&watch, 40, "alsa_output.usb-DAC", "Audio/Sink");
	sink_watch_reconcile(&watch, NULL);
	assert(strcmp(state.selected, "usb") == 0 && strcmp(state.sink_id, "30") == 0);
	long size = helper_log_size();
	add_node(&watch, 31, "alsa_input.usb-Headset", "Audio/Source");
	sink_watch_reconcile(&watch, NULL);
	assert(state.usb_microphone_available);
	assert(helper_log_size() == size); /* Capture must not reset playback gain. */
	sink_watch_global_remove(&watch, 31);
	sink_watch_reconcile(&watch, NULL);
	assert(!state.usb_microphone_available && helper_log_size() == size);

	snprintf(fixture, sizeof(fixture), "{%s,"
		"\"40\":{\"node.name\":\"alsa_output.usb-DAC\",\"media.class\":\"Audio/Sink\"}}", analog);
	assert(setenv("PW_FIXTURE", fixture, 1) == 0);
	sink_watch_global_remove(&watch, 30);
	sink_watch_reconcile(&watch, NULL);
	assert(strcmp(state.applied_route, "usb") == 0 && strcmp(state.sink_id, "40") == 0);
	assert(helper_log_size() > size);

	strcpy(state.mode, "usb");
	snprintf(fixture, sizeof(fixture), "{%s}", analog);
	assert(setenv("PW_FIXTURE", fixture, 1) == 0);
	sink_watch_global_remove(&watch, 40);
	sink_watch_reconcile(&watch, NULL);
	assert(strcmp(state.selected, "speaker") == 0 && strcmp(state.mode, "usb") == 0);
	snprintf(fixture, sizeof(fixture), "{%s,"
		"\"50\":{\"node.name\":\"alsa_output.usb-Headset\",\"media.class\":\"Audio/Sink\"}}", analog);
	assert(setenv("PW_FIXTURE", fixture, 1) == 0);
	add_node(&watch, 50, "alsa_output.usb-Headset", "Audio/Sink");
	sink_watch_reconcile(&watch, NULL);
	assert(strcmp(state.selected, "usb") == 0 && strcmp(state.sink_id, "50") == 0);
	sink_watch_close(&watch);
}

int main(void)
{
	test_policy();
	test_hotplug();
	test_volume_and_contract();
	test_registry_routing();
	puts("[PASS] audio policy, USB registry hotplug, volume persistence/mute, D-Bus compatibility");
	return 0;
}
