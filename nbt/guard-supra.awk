# Build from the pinned Supra source without adopting or removing its contexts.
# Fail the build if an upstream change prevents applying every guard.
module == "video_manager" && /mmr_launch_[.]adopt\(\);/ {
    adopted++
    next
}
module == "video_manager" && /\/\/ Purge any stale contexts currently holding/ {
    print "    std::string factory;"
    print "    if (!carlife::rendererAvailable(&factory)) return false;"
    print "    if (!factory.empty() && !detachFactoryContext(factory.c_str())) return false;"
    purge++
    skip_video = 1
    next
}
module == "video_manager" && /readCtxSnapshot\(name, snap\);/ {
    print
    print "    if (!snap.hasInput() || snap.outputIds.size() != 1 || !snap.hasOutput() ||"
    print "        !carlife::factoryProjectionHidden()) {"
    print "        puts(\"Could not verify idle CarPlay decoder and its restore URLs.\");"
    print "        return false;"
    print "    }"
    snapshot++
    next
}
module == "video_manager" && /int r_stop = fn_stop \? fn_stop\(cp_ctx_\)/ {
    print "    if (!carlife::beginFactoryBorrow()) return false;"
    borrow++
}
module == "video_manager" && /return attached > 0;/ {
    print "    if (attached > 0 && static_cast<size_t>(attached) == outs.size() &&"
    print "        (!cp_snap_.hasInput() || r_in >= 0))"
    print "        carlife::factoryRestored();"
    restored++
}
skip_video && /usleep\(50000\); \/\/ Allow QNX kernel/ { skip_video = 0 }
skip_video { next }
module == "video_manager" && /^#include "video_manager.hpp"/ {
    print
    print "#include \"renderer_guard.hpp\""
    next
}
module == "video_manager" && /kAaSurfaceFile =/ {
    sub("nbt-aad-surface", "nbt-carlife-surface")
    surface++
}
module == "video_manager" && /snprintf\(ctx_name,.*aadvideoctx/ {
    sub("aadvideoctx", "clvideoctx")
    context++
}
module == "video_manager" && /^[[:space:]]*"screen:\/any\?.*dstx=0&dsty=0&dstw=%d&dsth=%d&wingrp=%s"/ {
    sub(/dstx=0/, "dstx=%d")
    sub(/dsty=0/, "dsty=%d")
    positioned++
}
module == "video_manager" && /aaWindowId\(\), vidEnvInt\("NBT_VID_DST_W"/ {
    sub(/aaWindowId\(\), /, "aaWindowId(), carlife::outputX(), carlife::outputY(), ")
}
module == "video_manager" && /char lm\[256\];/ {
    # Screen already starts our window hidden. The logical surface can be
    # registered later without a visibility RPC against an unready renderer.
    startup_visibility++
    skip_startup_visibility = 1
}
skip_startup_visibility && /system\(lm\);/ { skip_startup_visibility = 0; next }
skip_startup_visibility { next }
module == "video_manager" && /system\("\/opt\/sys\/bin\/LayerManagerControl set surface 256 visibility 1/ {
    stop_visibility++
    next
}
module == "video_manager" && /^void VideoManager::setVisible\(bool visible\)/ {
    in_visibility = 1
}
in_visibility && /^}/ { in_visibility = 0 }
in_visibility && /\/\/ All LayerManager steps in ONE shell/ {
    print "    if (!visible) {"
    print "        hideVideoWindow();"
    print "        return;"
    print "    }"
    print "    if (aa_surface_logged_) return;"
    direct_hide++
}
in_visibility && /^    } else \{/ {
    skip_layer_hide = 1
    layer_hide++
}
skip_layer_hide && /^    }$/ { skip_layer_hide = 0 }
skip_layer_hide { next }
in_visibility && /"%s set surface 256 visibility %d 2>\/dev\/null; "/ {
    composition_visibility++
    next
}
in_visibility {
    native_behind += sub(/render order %s 2>\/dev\/null/, "render order %s,0x100 2>/dev/null")
    native_visible += sub(/L, favourite_preview_ \? 1 : 0, L, aa\);/, "L, aa);")
}
module == "video_manager" && /mmr_win_ = w;/ {
    print
    print "            mmr_hidden_ = false;"
    print "            if (!sink_.is_visible) hideVideoWindow();"
    late_window++
    skip_window_reset = 1
    next
}
skip_window_reset {
    skip_window_reset = 0
    if (/^            mmr_hidden_ = false;$/) {
        late_window_reset++
        next
    }
}
module == "video_manager" {
    if ($0 !~ /^[[:space:]]*\/\//) {
        command_run += gsub(/system\(/, "carlife::displayRun(")
        if ($0 ~ /popen\(/) {
            command_capture += sub(/popen\(/, "carlife::displayCapture(")
            capture_mode += sub(/, "r"\)/, ")")
        }
        command_close += gsub(/pclose\(/, "fclose(")
    }
    canvas += gsub(/nbt::videoCanvasWidth\(\)/, "carlife::videoWidth()")
    canvas += gsub(/nbt::videoCanvasHeight\(\)/, "carlife::videoHeight()")
    geometry += gsub(/nbt::mmrDstWidth\(\)/, "carlife::outputWidth()")
    geometry += gsub(/nbt::mmrDstHeight\(\)/, "carlife::outputHeight()")
}
module == "touch_probe" && /^#include "touch_probe.hpp"/ {
    print
    print "#include \"renderer_guard.hpp\""
    next
}
module == "touch_probe" {
    touch_geometry += sub(/const double panel = touchScale\(\);/,
                          "const double panel = carlife::liveDisplayRequested() ? 1.0 : touchScale();")
    touch_geometry += gsub(/videoCanvasWidth\(\)/, "(carlife::liveDisplayRequested() ? carConfig().screenW : videoCanvasWidth())")
    touch_geometry += gsub(/videoCanvasHeight\(\)/, "(carlife::liveDisplayRequested() ? carConfig().screenH : videoCanvasHeight())")
}
module == "touch_probe" && /^void savePeerFile\(const sockaddr_in& p\) \{/ {
    print "void savePeerFile(const sockaddr_in& p) { (void)p; }"
    touch++
    skip_touch = 1
    next
}
skip_touch && /^}/ { skip_touch = 0; next }
skip_touch { next }
{ print }
END {
    if (module == "video_manager" && (adopted != 1 || purge != 1 ||
        surface != 1 || context != 1 || snapshot != 1 || borrow != 1 || restored != 1 || positioned != 1 ||
        canvas != 6 || geometry != 2 || skip_video || startup_visibility != 1 ||
        stop_visibility != 1 || direct_hide != 1 || native_behind != 1 || native_visible != 1 ||
        composition_visibility != 1 ||
        late_window != 1 || late_window_reset != 1 || layer_hide != 1 || skip_startup_visibility ||
        command_run != 9 || command_capture != 6 || capture_mode != 6 || command_close != 6)) exit 1
    if (module == "touch_probe" && (touch != 1 || touch_geometry != 3 || skip_touch)) exit 1
}
