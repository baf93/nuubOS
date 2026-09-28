#!/usr/bin/env bash
set -uo pipefail
AREA="H3"
source "$NUBOS_ROOT/tests/platform/lib/common.sh"

remote '[ -e /dev/video0 ]' \
    && pass "/dev/video0 present" \
    || fail "/dev/video0 missing"

remote '[ -e /dev/media0 ]' \
    && pass "/dev/media0 present" \
    || fail "/dev/media0 missing"

name="$(remote 'cat /sys/class/video4linux/video0/name 2>/dev/null' || true)"
[[ "$name" == "cedrus" ]] \
    && pass "video0 driver is cedrus" \
    || fail "video0 driver '${name:-unknown}'"

if remote "find /sys/kernel/iommu_groups -type l -name '$CEDRUS_IOMMU_DEVICE' | grep -q ."; then
    pass "Cedrus attached to IOMMU"
else
    fail "Cedrus not attached to IOMMU"
fi

if remote "find /sys/kernel/iommu_groups -type l -name '$DISPLAY_IOMMU_DEVICE' | grep -q ."; then
    pass "Display mixer attached to IOMMU"
else
    fail "Display mixer not attached to IOMMU"
fi

faults="$(iommu_fault_count)"
[[ "$faults" == "0" ]] \
    && pass "IOMMU page faults = 0" \
    || fail "IOMMU page faults = $faults"

if remote_has v4l2-ctl; then
    formats="$(remote 'v4l2-ctl -d /dev/video0 --list-formats-out 2>/dev/null' || true)"
    for fmt in MG2S S264 S265 VP8F; do
        grep -q "'$fmt'" <<<"$formats" \
            && pass "V4L2 output format $fmt" \
            || fail "V4L2 output format $fmt missing"
    done
else
    skip "v4l2-ctl unavailable"
fi

if [[ "$NUBOS_TEST_MODE" == "full" ||
      "$NUBOS_TEST_MODE" == "stress" ||
      "$NUBOS_TEST_MODE" == "perf" ||
      "$NUBOS_TEST_MODE" == "H3" ]]; then
    if remote_has v4l2-compliance; then
        compliance="$(remote 'v4l2-compliance -d /dev/video0 2>&1' || true)"
        summary="$(grep 'Total for cedrus' <<<"$compliance" | tail -1)"

        if grep -q 'Failed: 0' <<<"$summary"; then
            pass "v4l2-compliance clean"
        elif grep -q 'Failed: 1' <<<"$summary" && grep -q 'VIDIOC_G/S/TRY_EXT_CTRLS: FAIL' <<<"$compliance"; then
            warn "v4l2-compliance known stateless TRY_EXT_CTRLS quirk (1 failure)"
        else
            fail "unexpected v4l2-compliance result"
            printf '%s\n' "$summary"
        fi
    else
        skip "v4l2-compliance unavailable"
    fi
fi

# Dedicated H3 performance mode.
#
# This is intentionally strict about its diagnostic prerequisites because
# invoking `perf` explicitly means we want a real DMA-BUF performance result.
if [[ "$NUBOS_TEST_MODE" == "perf" ]]; then
    if ! remote_has gst-launch-1.0 || ! remote_has gst-inspect-1.0; then
        fail "perf prerequisites missing: GStreamer tools unavailable"
        exit 0
    fi

    for element in v4l2slh264dec v4l2slh265dec fakevideosink; do
        if ! remote "gst-inspect-1.0 '$element' >/dev/null 2>&1"; then
            fail "perf prerequisite missing: $element"
            exit 0
        fi
    done

    perf_vectors=(
        h264-high-1080p60-10s.h264
        hevc-main-1080p60-10s.hevc
        hevc-main10-1080p60-10s.hevc
    )

    if ! ensure_test_vectors "${perf_vectors[@]}"; then
        fail "perf test vectors unavailable"
        exit 0
    fi

    for f in "${perf_vectors[@]}"; do
        if sync_test_vector "$f"; then
            pass "perf vector synced: $f"
        else
            fail "perf vector transfer failed: $f"
            exit 0
        fi
    done

    run_dmabuf_perf() {
        local file="$1"
        local parser="$2"
        local caps="$3"
        local decoder="$4"
        local frames="$5"
        local expected_irq="$6"
        local min_fps="$7"
        local label="$8"

        local before after delta rc start_ns end_ns elapsed_ns fps

        before="$(irq_sum '1c0e000.video-codec')"
        before="${before:-0}"

        start_ns="$(date +%s%N)"
        remote "
gst-launch-1.0 -q \
  filesrc location='/tmp/$file' \
  ! $parser \
  ! '$caps' \
  ! $decoder video-device=/dev/video0 media-device=/dev/media0 \
  ! 'video/x-raw(memory:DMABuf),format=DMA_DRM,drm-format=NV12' \
  ! fakevideosink sync=false qos=false enable-last-sample=false
"
        rc=$?
        end_ns="$(date +%s%N)"

        after="$(irq_sum '1c0e000.video-codec')"
        after="${after:-0}"
        delta=$((after - before))
        elapsed_ns=$((end_ns - start_ns))

        fps="$(awk -v f="$frames" -v ns="$elapsed_ns" \
            'BEGIN { if (ns <= 0) print "0.0"; else printf "%.1f", f * 1000000000 / ns }')"

        if ((rc != 0)); then
            fail "$label DMA-BUF benchmark failed (rc=$rc)"
            return 1
        fi

        if ((delta != expected_irq)); then
            fail "$label IRQ=$delta expected=$expected_irq"
            return 1
        fi

        if awk -v fps="$fps" -v min="$min_fps" 'BEGIN { exit !(fps >= min) }'; then
            pass "$label DMA-BUF ${fps} fps (threshold ${min_fps} fps, IRQ $delta/$expected_irq)"
            return 0
        fi

        fail "$label DMA-BUF ${fps} fps below ${min_fps} fps threshold"
        return 1
    }

    run_dmabuf_perf \
        h264-high-1080p60-10s.h264 \
        h264parse \
        "video/x-h264,stream-format=byte-stream,alignment=au" \
        v4l2slh264dec \
        600 600 60 \
        "H.264 High 1080p60"

    run_dmabuf_perf \
        hevc-main-1080p60-10s.hevc \
        h265parse \
        "video/x-h265,stream-format=byte-stream,alignment=au" \
        v4l2slh265dec \
        600 600 60 \
        "HEVC Main 1080p60"

    run_dmabuf_perf \
        hevc-main10-1080p60-10s.hevc \
        h265parse \
        "video/x-h265,stream-format=byte-stream,alignment=au" \
        v4l2slh265dec \
        600 600 60 \
        "HEVC Main10 1080p60"

    faults_after="$(iommu_fault_count)"
    [[ "$faults_after" == "0" ]] \
        && pass "post-perf IOMMU page faults = 0" \
        || fail "post-perf IOMMU page faults = $faults_after"

    ve="$(ve_clocks_idle)"
    if remote '
mount -t debugfs none /sys/kernel/debug 2>/dev/null || true
awk '"'"'
$1=="pll-ve" || $1=="ve" || $1=="bus-ve" || $1=="mbus-ve" {
    found++;
    if ($2 != 0 || $3 != 0) bad++;
}
END { exit !(found == 4 && bad == 0) }
'"'"' /sys/kernel/debug/clk/clk_summary
'; then
        pass "VE runtime-PM idle after perf"
    else
        warn "unable to confirm VE idle state after perf"
        printf '%s\n' "$ve"
    fi

    exit 0
fi

# Production images are allowed not to contain the GStreamer diagnostic stack.
if ! remote_has gst-launch-1.0 || ! remote_has gst-inspect-1.0; then
    skip "GStreamer diagnostic decode tests unavailable"
    exit 0
fi

if ! remote 'gst-inspect-1.0 v4l2slh264dec >/dev/null 2>&1'; then
    skip "v4l2slh264dec unavailable"
    exit 0
fi

if [[ "$NUBOS_TEST_MODE" == "quick" ]]; then
    vectors=(h264-baseline-320x240-30-2s.h264)

    if ! ensure_test_vectors "${vectors[@]}"; then
        skip "H.264 quick decode (test vector unavailable)"
        exit 0
    fi

    sync_test_vector "${vectors[0]}" || {
        fail "failed to transfer H.264 quick vector"
        exit 0
    }

    run_decode_test \
      "${vectors[0]}" \
      h264parse \
      "video/x-h264,stream-format=byte-stream,alignment=au" \
      v4l2slh264dec \
      60 \
      "H.264 baseline 320x240@30"

    exit 0
fi

full_vectors=(
    h264-high-720p30-10s.h264
    h264-high-1080p30-10s.h264
    h264-high-1080p60-10s.h264
    hevc-main-720p30-10s.hevc
    hevc-main10-720p30-10s.hevc
    hevc-main-1080p30-10s.hevc
    hevc-main10-1080p30-10s.hevc
)

if ! ensure_test_vectors "${full_vectors[@]}"; then
    skip "full codec decode tests (test vectors unavailable)"
    exit 0
fi

for f in "${full_vectors[@]}"; do
    if sync_test_vector "$f"; then
        pass "test vector synced: $f"
    else
        fail "test vector transfer failed: $f"
        exit 0
    fi
done

if [[ "$NUBOS_TEST_MODE" == "stress" ]]; then
    failures=0

    for n in $(seq 1 10); do
        echo "H3 stress round $n/10"

        run_decode_test \
          h264-high-1080p60-10s.h264 \
          h264parse \
          "video/x-h264,stream-format=byte-stream,alignment=au" \
          v4l2slh264dec \
          600 \
          "stress[$n] H.264 1080p60" || failures=$((failures+1))

        run_decode_test \
          hevc-main-1080p30-10s.hevc \
          h265parse \
          "video/x-h265,stream-format=byte-stream,alignment=au" \
          v4l2slh265dec \
          300 \
          "stress[$n] HEVC Main 1080p30" || failures=$((failures+1))

        run_decode_test \
          hevc-main10-1080p30-10s.hevc \
          h265parse \
          "video/x-h265,stream-format=byte-stream,alignment=au" \
          v4l2slh265dec \
          300 \
          "stress[$n] HEVC Main10 1080p30" || failures=$((failures+1))
    done

    [[ "$failures" == "0" ]] \
        && pass "codec stress completed without failures" \
        || fail "codec stress failures=$failures"
else
    run_decode_test h264-high-720p30-10s.h264 h264parse \
        "video/x-h264,stream-format=byte-stream,alignment=au" \
        v4l2slh264dec 300 "H.264 High 720p30"

    run_decode_test h264-high-1080p30-10s.h264 h264parse \
        "video/x-h264,stream-format=byte-stream,alignment=au" \
        v4l2slh264dec 300 "H.264 High 1080p30"

    run_decode_test h264-high-1080p60-10s.h264 h264parse \
        "video/x-h264,stream-format=byte-stream,alignment=au" \
        v4l2slh264dec 600 "H.264 High 1080p60"

    run_decode_test hevc-main-720p30-10s.hevc h265parse \
        "video/x-h265,stream-format=byte-stream,alignment=au" \
        v4l2slh265dec 300 "HEVC Main 720p30"

    run_decode_test hevc-main10-720p30-10s.hevc h265parse \
        "video/x-h265,stream-format=byte-stream,alignment=au" \
        v4l2slh265dec 300 "HEVC Main10 720p30"

    run_decode_test hevc-main-1080p30-10s.hevc h265parse \
        "video/x-h265,stream-format=byte-stream,alignment=au" \
        v4l2slh265dec 300 "HEVC Main 1080p30"

    run_decode_test hevc-main10-1080p30-10s.hevc h265parse \
        "video/x-h265,stream-format=byte-stream,alignment=au" \
        v4l2slh265dec 300 "HEVC Main10 1080p30"
fi

faults_after="$(iommu_fault_count)"
[[ "$faults_after" == "0" ]] \
    && pass "post-decode IOMMU page faults = 0" \
    || fail "post-decode IOMMU page faults = $faults_after"

ve="$(ve_clocks_idle)"
if remote '
mount -t debugfs none /sys/kernel/debug 2>/dev/null || true
awk '"'"'
$1=="pll-ve" || $1=="ve" || $1=="bus-ve" || $1=="mbus-ve" {
    found++;
    if ($2 != 0 || $3 != 0) bad++;
}
END { exit !(found == 4 && bad == 0) }
'"'"' /sys/kernel/debug/clk/clk_summary
'; then
    pass "VE runtime-PM idle after decode"
else
    warn "unable to confirm VE idle state"
    printf '%s\n' "$ve"
fi
