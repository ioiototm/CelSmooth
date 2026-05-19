#include "celsmooth/mlaa.h"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include <vector>
#include <string>
#include <chrono>
#include <cstdio>
#include <algorithm>
#include <cstring>

#include "fonts/inter_medium.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>
#endif

// --- PNG color profile preservation ---

struct PngColorProfile {
    std::vector<uint8_t> chunks;  // raw color-related PNG chunks
};

static PngColorProfile extract_png_color_profile(const char* path) {
    PngColorProfile profile;
    FILE* f = fopen(path, "rb");
    if (!f) return profile;

    uint8_t sig[8];
    if (fread(sig, 1, 8, f) != 8) { fclose(f); return profile; }
    const uint8_t png_sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    if (memcmp(sig, png_sig, 8) != 0) { fclose(f); return profile; }

    while (!feof(f)) {
        uint8_t hdr[8];
        if (fread(hdr, 1, 8, f) != 8) break;
        uint32_t length = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16)
                        | ((uint32_t)hdr[2] << 8)  |  (uint32_t)hdr[3];
        char type[5] = { (char)hdr[4], (char)hdr[5], (char)hdr[6], (char)hdr[7], 0 };

        std::vector<uint8_t> body(length + 4);
        if (fread(body.data(), 1, length + 4, f) != length + 4) break;

        if (strcmp(type, "iCCP") == 0 || strcmp(type, "sRGB") == 0 ||
            strcmp(type, "gAMA") == 0 || strcmp(type, "cHRM") == 0) {
            profile.chunks.insert(profile.chunks.end(), hdr, hdr + 8);
            profile.chunks.insert(profile.chunks.end(), body.begin(), body.end());
        }

        if (strcmp(type, "IDAT") == 0) break;
    }
    fclose(f);
    return profile;
}

static std::vector<uint8_t> make_srgb_chunk() {
    uint8_t td[5] = { 's', 'R', 'G', 'B', 0x00 };
    uint32_t crc = 0xFFFFFFFF;
    for (int i = 0; i < 5; i++) {
        crc ^= td[i];
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (0xEDB88320u & (-(crc & 1)));
    }
    crc ^= 0xFFFFFFFF;
    return { 0,0,0,1, 's','R','G','B', 0x00,
             (uint8_t)(crc>>24), (uint8_t)(crc>>16),
             (uint8_t)(crc>>8),  (uint8_t)(crc) };
}

static void inject_color_profile(std::vector<uint8_t>& png, const PngColorProfile& profile) {
    if (png.size() < 33) return;
    if (!profile.chunks.empty()) {
        png.insert(png.begin() + 33, profile.chunks.begin(), profile.chunks.end());
    } else {
        auto srgb = make_srgb_chunk();
        png.insert(png.begin() + 33, srgb.begin(), srgb.end());
    }
}

static bool save_png_with_profile(const char* path, int w, int h, int comp,
                                  const void* data, int stride,
                                  const PngColorProfile& profile) {
    std::vector<uint8_t> png;
    int ok = stbi_write_png_to_func([](void* ctx, void* d, int sz) {
        auto* v = static_cast<std::vector<uint8_t>*>(ctx);
        v->insert(v->end(), static_cast<uint8_t*>(d), static_cast<uint8_t*>(d) + sz);
    }, &png, w, h, comp, data, stride);
    if (!ok) return false;

    inject_color_profile(png, profile);

    FILE* f = fopen(path, "wb");
    if (!f) return false;
    size_t written = fwrite(png.data(), 1, png.size(), f);
    fclose(f);
    return written == png.size();
}

// --- Application state ---

struct AppState {
    // Image data
    std::vector<uint8_t> pixels_in;   // RGBA input
    std::vector<uint8_t> pixels_out;  // RGBA output
    int img_w = 0, img_h = 0;
    std::string img_path;

    // SDL textures
    SDL_Texture* tex_in  = nullptr;
    SDL_Texture* tex_out = nullptr;
    SDL_Renderer* renderer = nullptr;

    // Parameters
    celsmooth::MlaaParams params;
    bool params_dirty = true;  // reprocess on next frame

    // Timing
    double last_process_ms = 0.0;

    // View
    float zoom = 1.0f;
    float pan_x = 0.0f, pan_y = 0.0f;
    bool show_original = false;  // hold to show original

    // Compare mode: 0 = toggle (Space), 1 = split wipe
    int compare_mode = 0;
    float split_pos = 0.5f;       // 0..1 normalized position of the divider
    bool dragging_split = false;

    // Save
    std::string last_save_path;
    double save_flash_timer = 0.0;  // countdown for "Saved!" message

    // Color profile (copied from input PNG)
    PngColorProfile color_profile;

    // Sidebar width
    float sidebar_w = 310.0f;
};

static std::string make_output_path(const std::string& input_path) {
    // "image.png" -> "image_celsmooth.png"
    std::string out = input_path;
    size_t dot = out.rfind('.');
    if (dot != std::string::npos)
        out.insert(dot, "_celsmooth");
    else
        out += "_celsmooth.png";
    return out;
}

static void update_texture(SDL_Texture*& tex, SDL_Renderer* renderer, const uint8_t* pixels, int w, int h) {
    if (tex) {
        float tw = 0, th = 0;
        SDL_GetTextureSize(tex, &tw, &th);
        if ((int)tw != w || (int)th != h) {
            SDL_DestroyTexture(tex);
            tex = nullptr;
        }
    }
    if (!tex) {
        tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, w, h);
    }
    if (tex) {
        SDL_UpdateTexture(tex, nullptr, pixels, w * 4);
    }
}

static bool load_image(AppState& state, const char* path) {
    int w, h, channels;
    uint8_t* data = stbi_load(path, &w, &h, &channels, 4);
    if (!data) return false;

    state.img_w = w;
    state.img_h = h;
    state.img_path = path;
    state.color_profile = extract_png_color_profile(path);
    state.pixels_in.assign(data, data + (size_t)w * h * 4);
    state.pixels_out.resize((size_t)w * h * 4);
    stbi_image_free(data);

    update_texture(state.tex_in, state.renderer, state.pixels_in.data(), w, h);
    state.params_dirty = true;
    state.zoom = 1.0f;
    state.pan_x = 0.0f;
    state.pan_y = 0.0f;
    return true;
}

static void process_image(AppState& state) {
    if (state.pixels_in.empty()) return;

    auto t0 = std::chrono::high_resolution_clock::now();
    celsmooth::mlaa_process(
        state.pixels_in.data(), state.pixels_out.data(),
        state.img_w, state.img_h, state.img_w * 4,
        state.params);
    auto t1 = std::chrono::high_resolution_clock::now();
    state.last_process_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    update_texture(state.tex_out, state.renderer, state.pixels_out.data(), state.img_w, state.img_h);
    state.params_dirty = false;
}

// --- WASM file I/O ---

#ifdef __EMSCRIPTEN__
static AppState* g_wasm_state = nullptr;

extern "C" EMSCRIPTEN_KEEPALIVE void wasm_receive_file(const char* path) {
    if (g_wasm_state) {
        load_image(*g_wasm_state, path);
    }
}

static void open_file_dialog() {
    EM_ASM({
        var input = document.createElement('input');
        input.type = 'file';
        input.accept = 'image/*';
        input.onchange = function(e) {
            var file = e.target.files[0];
            if (!file) return;
            var reader = new FileReader();
            reader.onload = function() {
                var data = new Uint8Array(reader.result);
                var path = '/tmp/' + file.name;
                try { FS.unlink(path); } catch(e) {}
                FS.writeFile(path, data);
                var pathPtr = stringToNewUTF8(path);
                Module._wasm_receive_file(pathPtr);
                _free(pathPtr);
            };
            reader.readAsArrayBuffer(file);
        };
        input.click();
    });
}

static void wasm_download_image(AppState& state) {
    // Encode PNG to memory
    struct PngBuf { std::vector<uint8_t> data; };
    PngBuf buf;
    stbi_write_png_to_func([](void* ctx, void* data, int size) {
        auto* b = static_cast<PngBuf*>(ctx);
        auto* bytes = static_cast<uint8_t*>(data);
        b->data.insert(b->data.end(), bytes, bytes + size);
    }, &buf, state.img_w, state.img_h, 4, state.pixels_out.data(), state.img_w * 4);

    // Extract filename for download
    std::string filename = "celsmooth_output.png";
    if (!state.img_path.empty()) {
        size_t slash = state.img_path.find_last_of("/\\");
        std::string base = (slash != std::string::npos)
            ? state.img_path.substr(slash + 1) : state.img_path;
        size_t dot = base.rfind('.');
        if (dot != std::string::npos)
            base.insert(dot, "_celsmooth");
        else
            base += "_celsmooth.png";
        filename = base;
    }

    // Preserve the original image's color profile
    inject_color_profile(buf.data, state.color_profile);

    // Trigger browser download
    EM_ASM({
        var data = new Uint8Array(HEAPU8.buffer, $0, $1);
        var blob = new Blob([data.slice()], {type: 'image/png'});
        var url = URL.createObjectURL(blob);
        var a = document.createElement('a');
        a.href = url;
        a.download = UTF8ToString($2);
        document.body.appendChild(a);
        a.click();
        document.body.removeChild(a);
        URL.revokeObjectURL(url);
    }, buf.data.data(), (int)buf.data.size(), filename.c_str());
}
#endif

// Helper: full-bleed section header — custom drawn, no CollapsingHeader
static bool SectionHeader(const char* label) {
    ImGuiID id = ImGui::GetID(label);
    ImGuiStorage* storage = ImGui::GetStateStorage();

    // Default to open
    bool open = storage->GetBool(id, true);

    ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImVec2 win_pos = ImGui::GetWindowPos();
    float win_w = ImGui::GetWindowSize().x;
    float h = ImGui::GetFrameHeight();
    float pad_x = ImGui::GetStyle().WindowPadding.x;

    ImVec2 p0(win_pos.x, cursor.y);
    ImVec2 p1(win_pos.x + win_w, cursor.y + h);

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Hit test
    bool hovered = ImGui::IsMouseHoveringRect(p0, p1);
    bool clicked = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);

    if (clicked) {
        open = !open;
        storage->SetBool(id, open);
    }

    // Background
    ImU32 bg_col = hovered ? IM_COL32(50, 50, 56, 255) : IM_COL32(38, 38, 43, 255);
    dl->AddRectFilled(p0, p1, bg_col);

    // Triangle arrow
    float arrow_size = 8.0f;
    float arrow_x = cursor.x;
    float arrow_cy = cursor.y + h * 0.5f;
    if (open) {
        // Down arrow
        dl->AddTriangleFilled(
            ImVec2(arrow_x, arrow_cy - arrow_size * 0.35f),
            ImVec2(arrow_x + arrow_size, arrow_cy - arrow_size * 0.35f),
            ImVec2(arrow_x + arrow_size * 0.5f, arrow_cy + arrow_size * 0.35f),
            IM_COL32(140, 140, 150, 255));
    } else {
        // Right arrow
        dl->AddTriangleFilled(
            ImVec2(arrow_x, arrow_cy - arrow_size * 0.5f),
            ImVec2(arrow_x + arrow_size * 0.7f, arrow_cy),
            ImVec2(arrow_x, arrow_cy + arrow_size * 0.5f),
            IM_COL32(140, 140, 150, 255));
    }

    // Label
    float text_x = arrow_x + arrow_size + 8.0f;
    float text_y = cursor.y + (h - ImGui::GetTextLineHeight()) * 0.5f;
    dl->AddText(ImVec2(text_x, text_y), IM_COL32(220, 220, 225, 255), label);

    // Advance cursor past the header
    ImGui::SetCursorScreenPos(ImVec2(cursor.x, cursor.y + h + ImGui::GetStyle().ItemSpacing.y));

    return open;
}

// Helper: label above a slider, full width
static bool LabeledSliderFloat(const char* label, const char* tooltip, float* v, float v_min, float v_max, const char* fmt = "%.2f") {
    ImGui::TextDisabled("%s", label);
    ImGui::SetNextItemWidth(-1);
    // Use ## to hide the label from the slider itself (we already drew it above)
    char id[128];
    snprintf(id, sizeof(id), "##%s", label);
    bool changed = ImGui::SliderFloat(id, v, v_min, v_max, fmt);
    if (ImGui::IsItemHovered() && tooltip)
        ImGui::SetTooltip("%s", tooltip);
    return changed;
}

static bool LabeledSliderInt(const char* label, const char* tooltip, int* v, int v_min, int v_max) {
    ImGui::TextDisabled("%s", label);
    ImGui::SetNextItemWidth(-1);
    char id[128];
    snprintf(id, sizeof(id), "##%s", label);
    bool changed = ImGui::SliderInt(id, v, v_min, v_max);
    if (ImGui::IsItemHovered() && tooltip)
        ImGui::SetTooltip("%s", tooltip);
    return changed;
}

static void draw_controls(AppState& state) {
    // Drawn inside a child region — no Begin/End needed
    // Push item width to leave a right margin so nothing hugs the splitter
    ImGui::PushItemWidth(-8);

#ifdef __EMSCRIPTEN__
    if (ImGui::Button("Open Image...", ImVec2(-1, 0))) {
        open_file_dialog();
    }
    ImGui::Spacing();
#endif

    // --- Image info ---
    if (state.img_w > 0) {
        // Filename only (not full path)
        std::string fname = state.img_path;
        size_t slash = fname.find_last_of("/\\");
        if (slash != std::string::npos) fname = fname.substr(slash + 1);

        ImGui::TextColored(ImVec4(0.75f, 0.75f, 0.75f, 1.0f), "%s", fname.c_str());
        ImGui::TextDisabled("%d x %d  |  %.1f ms", state.img_w, state.img_h, state.last_process_ms);
        ImGui::Spacing();
    }

    // --- Core Parameters ---
    if (SectionHeader("Core")) {
        ImGui::Spacing();

        if (LabeledSliderFloat("Threshold", "Edge detection sensitivity.\nLower = more edges detected.\nHigher = fewer edges.",
                &state.params.threshold, 0.01f, 0.5f, "%.3f"))
            state.params_dirty = true;

        ImGui::Spacing();

        int search_dist = state.params.max_distance;
        if (LabeledSliderInt("Search Distance", "Maximum edge segment length.\nLonger = handles longer straight edges.\nShorter = faster.",
                &search_dist, 2, 128)) {
            state.params.max_distance = search_dist;
            state.params_dirty = true;
        }

        ImGui::Spacing();

        if (LabeledSliderFloat("Strength", "Blend intensity.\n1.0 = full anti-aliasing.\n0.5 = subtle.\n>1.0 = exaggerated.",
                &state.params.strength, 0.0f, 1.5f))
            state.params_dirty = true;

        ImGui::Spacing();
    }

    // --- V2 Features ---
    if (SectionHeader("V2 Features")) {
        ImGui::Spacing();

        bool classic = state.params.classic_mode;
        if (ImGui::Checkbox("Classic Mode (V1 only)", &classic)) {
            state.params.classic_mode = classic;
            state.params_dirty = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Disable all V2 features.\nUses the original 2009 MLAA algorithm only.");

        if (!state.params.classic_mode) {
            ImGui::Spacing();

            if (ImGui::Checkbox("T/Cross Shapes", &state.params.enable_t_cross))
                state.params_dirty = true;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Handle T-junctions and cross-shaped intersections.");

            if (ImGui::Checkbox("Diagonal Detection", &state.params.enable_diagonals))
                state.params_dirty = true;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Smooth 45-degree staircase patterns.");

            if (ImGui::Checkbox("Gamma Correction", &state.params.enable_gamma))
                state.params_dirty = true;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Blend in linear light space (physically correct).");

            if (state.params.enable_gamma) {
                ImGui::Spacing();
                if (LabeledSliderFloat("Extended Gamma", "Boost thin dark strokes.\n1.0 = standard.\n>1.0 = preserves thin lines.",
                        &state.params.extended_gamma, 0.5f, 3.0f, "%.1f"))
                    state.params_dirty = true;
            }

            ImGui::Spacing();

            if (LabeledSliderFloat("Smoothness", "Blend falloff shape.\n1.0 = standard.\n<1.0 = sharper.\n>1.0 = smoother.",
                    &state.params.smoothness, 0.0f, 2.0f))
                state.params_dirty = true;

            ImGui::Spacing();

            if (ImGui::Checkbox("U-Rounding", &state.params.u_rounding))
                state.params_dirty = true;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Rounder corners where two edges meet on the same side.");
        }

        ImGui::Spacing();
    }

    // --- View ---
    if (SectionHeader("View")) {
        ImGui::Spacing();

        if (LabeledSliderFloat("Zoom", "Scroll wheel also zooms in the preview.", &state.zoom, 0.25f, 8.0f, "%.2fx")) {}

        ImGui::Spacing();

        ImGui::TextDisabled("Compare Mode");
        ImGui::SetNextItemWidth(-1);
        const char* compare_labels[] = { "Toggle (Space)", "Split Wipe" };
        ImGui::Combo("##Compare", &state.compare_mode, compare_labels, 2);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Toggle: hold Space to flash the original.\nSplit Wipe: drag a divider to compare side by side.");

        if (state.compare_mode == 0) {
            ImGui::TextDisabled("Hold SPACE to see original");
        } else {
            ImGui::TextDisabled("Drag the divider in the preview");
        }

        ImGui::Spacing();
    }

    // --- Actions ---
    ImGui::Spacing();
    if (ImGui::Button("Reset Parameters", ImVec2(-1, 0))) {
        state.params = celsmooth::MlaaParams{};
        state.params_dirty = true;
    }

    if (state.img_w > 0) {
        ImGui::Spacing();

#ifdef __EMSCRIPTEN__
        if (ImGui::Button("Download Result", ImVec2(-1, 0))) {
            wasm_download_image(state);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Download the processed image as PNG");
#else
        // Accent-colored save button
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.22f, 0.42f, 0.44f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  ImVec4(0.28f, 0.55f, 0.57f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,   ImVec4(0.32f, 0.65f, 0.67f, 1.0f));
        if (ImGui::Button("Save Result", ImVec2(-1, 0))) {
            std::string out_path = make_output_path(state.img_path);
            if (save_png_with_profile(out_path.c_str(), state.img_w, state.img_h, 4,
                                      state.pixels_out.data(), state.img_w * 4,
                                      state.color_profile)) {
                state.last_save_path = out_path;
                state.save_flash_timer = 3.0;
            }
        }
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Save as <filename>_celsmooth.png\nnext to the original");

        if (state.save_flash_timer > 0.0) {
            state.save_flash_timer -= ImGui::GetIO().DeltaTime;
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f), "Saved!");
            ImGui::TextWrapped("%s", state.last_save_path.c_str());
        }
#endif
    }

    ImGui::PopItemWidth();
}

static void draw_preview(AppState& state) {
    // Drawn inside a child region — no Begin/End needed

    if (state.img_w == 0) {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImVec2 cursor = ImGui::GetCursorScreenPos();

        // Dashed border rectangle
        float margin = 40.0f;
        if (avail.x > margin * 3 && avail.y > margin * 3) {
            ImVec2 r0(cursor.x + margin, cursor.y + margin);
            ImVec2 r1(cursor.x + avail.x - margin, cursor.y + avail.y - margin);
            ImDrawList* dl = ImGui::GetWindowDrawList();

            // Draw dashed border (series of short lines)
            ImU32 dash_col = IM_COL32(80, 80, 80, 160);
            float dash_len = 10.0f, gap_len = 8.0f;
            auto draw_dashed_line = [&](ImVec2 a, ImVec2 b) {
                float dx = b.x - a.x, dy = b.y - a.y;
                float len = sqrtf(dx * dx + dy * dy);
                if (len < 1.0f) return;
                dx /= len; dy /= len;
                float t = 0;
                while (t < len) {
                    float end = std::min(t + dash_len, len);
                    dl->AddLine(ImVec2(a.x + dx * t, a.y + dy * t),
                                ImVec2(a.x + dx * end, a.y + dy * end), dash_col, 1.5f);
                    t = end + gap_len;
                }
            };
            draw_dashed_line(r0, ImVec2(r1.x, r0.y));
            draw_dashed_line(ImVec2(r1.x, r0.y), r1);
            draw_dashed_line(r1, ImVec2(r0.x, r1.y));
            draw_dashed_line(ImVec2(r0.x, r1.y), r0);
        }

        // Centered hint text
#ifdef __EMSCRIPTEN__
        const char* hint = "Open or drop an image to begin";
#else
        const char* hint = "Drop an image here to begin";
#endif
        ImVec2 text_size = ImGui::CalcTextSize(hint);
        ImGui::SetCursorPos(ImVec2(
            (avail.x - text_size.x) * 0.5f,
            (avail.y - text_size.y) * 0.5f));
        ImGui::TextColored(ImVec4(0.40f, 0.40f, 0.42f, 1.0f), "%s", hint);

        return;
    }

    // Handle zoom with scroll wheel when hovered
    if (ImGui::IsWindowHovered()) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            float factor = (wheel > 0) ? 1.15f : 1.0f / 1.15f;
            state.zoom = std::clamp(state.zoom * factor, 0.25f, 8.0f);
        }
        // Pan with middle mouse button
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
            ImVec2 delta = ImGui::GetIO().MouseDelta;
            state.pan_x += delta.x;
            state.pan_y += delta.y;
        }
    }

    if (!state.tex_in || !state.tex_out) return;

    ImVec2 avail = ImGui::GetContentRegionAvail();
    float disp_w = state.img_w * state.zoom;
    float disp_h = state.img_h * state.zoom;

    // Center the image
    float offset_x = (avail.x - disp_w) * 0.5f + state.pan_x;
    float offset_y = (avail.y - disp_h) * 0.5f + state.pan_y;

    ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImVec2 p0(cursor.x + offset_x, cursor.y + offset_y);
    ImVec2 p1(p0.x + disp_w, p0.y + disp_h);

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Background
    dl->AddRectFilled(p0, p1, IM_COL32(40, 40, 40, 255));

    if (state.compare_mode == 0) {
        // --- Toggle mode: Space to flash original ---
        state.show_original = ImGui::IsKeyDown(ImGuiKey_Space);

        SDL_Texture* tex = state.show_original ? state.tex_in : state.tex_out;
        dl->AddImage((ImTextureID)tex, p0, p1);

        const char* label = state.show_original ? "ORIGINAL" : "PROCESSED";
        ImVec2 label_pos(cursor.x + 8, cursor.y + 4);
        dl->AddText(ImVec2(label_pos.x + 1, label_pos.y + 1), IM_COL32(0, 0, 0, 180), label);
        dl->AddText(label_pos, state.show_original ? IM_COL32(255, 200, 100, 255) : IM_COL32(100, 255, 200, 255), label);

    } else {
        // --- Split wipe mode ---
        // The divider position in screen pixels
        float split_x = p0.x + disp_w * state.split_pos;

        // Handle dragging the split line
        ImVec2 mouse = ImGui::GetIO().MousePos;
        float grab_zone = 8.0f;
        bool mouse_near_split = (mouse.x >= split_x - grab_zone && mouse.x <= split_x + grab_zone
                                 && mouse.y >= p0.y && mouse.y <= p1.y);

        if (mouse_near_split || state.dragging_split)
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && mouse_near_split && ImGui::IsWindowHovered()) {
            state.dragging_split = true;
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            state.dragging_split = false;
        }
        if (state.dragging_split) {
            state.split_pos = std::clamp((mouse.x - p0.x) / disp_w, 0.0f, 1.0f);
            split_x = p0.x + disp_w * state.split_pos;
        }

        // Left side: original (clipped to left of divider)
        // UV coordinates map split_pos to the texture
        ImVec2 uv0_left(0.0f, 0.0f);
        ImVec2 uv1_left(state.split_pos, 1.0f);
        ImVec2 p1_left(split_x, p1.y);
        dl->AddImage((ImTextureID)state.tex_in, p0, p1_left, uv0_left, uv1_left);

        // Right side: processed (clipped to right of divider)
        ImVec2 uv0_right(state.split_pos, 0.0f);
        ImVec2 uv1_right(1.0f, 1.0f);
        ImVec2 p0_right(split_x, p0.y);
        dl->AddImage((ImTextureID)state.tex_out, p0_right, p1, uv0_right, uv1_right);

        // Divider line
        dl->AddLine(ImVec2(split_x, p0.y), ImVec2(split_x, p1.y),
                    IM_COL32(255, 255, 255, 200), 2.0f);

        // Small handle triangle on the divider
        float handle_y = (p0.y + p1.y) * 0.5f;
        float hs = 8.0f; // handle size
        // Left-pointing triangle
        dl->AddTriangleFilled(
            ImVec2(split_x - hs, handle_y),
            ImVec2(split_x - 2, handle_y - hs),
            ImVec2(split_x - 2, handle_y + hs),
            IM_COL32(255, 200, 100, 220));
        // Right-pointing triangle
        dl->AddTriangleFilled(
            ImVec2(split_x + hs, handle_y),
            ImVec2(split_x + 2, handle_y - hs),
            ImVec2(split_x + 2, handle_y + hs),
            IM_COL32(100, 255, 200, 220));

        // Labels on each side
        ImVec2 lbl_orig(p0.x + 8, cursor.y + 4);
        dl->AddText(ImVec2(lbl_orig.x + 1, lbl_orig.y + 1), IM_COL32(0, 0, 0, 180), "ORIGINAL");
        dl->AddText(lbl_orig, IM_COL32(255, 200, 100, 255), "ORIGINAL");

        ImVec2 lbl_proc_size = ImGui::CalcTextSize("PROCESSED");
        ImVec2 lbl_proc(p1.x - lbl_proc_size.x - 8, cursor.y + 4);
        dl->AddText(ImVec2(lbl_proc.x + 1, lbl_proc.y + 1), IM_COL32(0, 0, 0, 180), "PROCESSED");
        dl->AddText(lbl_proc, IM_COL32(100, 255, 200, 255), "PROCESSED");
    }
}

// --- Common init (shared between desktop and WASM) ---

static SDL_Window* g_window = nullptr;
static SDL_Renderer* g_renderer = nullptr;
static AppState g_state;
static bool g_running = true;

static bool init_app(int argc, char* argv[]) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return false;
    }

    g_window = SDL_CreateWindow(
        "CelSmooth v" CELSMOOTH_VERSION,
        1280, 800,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!g_window) {
        SDL_Log("CreateWindow failed: %s", SDL_GetError());
        return false;
    }

    g_renderer = SDL_CreateRenderer(g_window, nullptr);
    if (!g_renderer) {
        SDL_Log("CreateRenderer failed: %s", SDL_GetError());
        return false;
    }

    // ImGui setup
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;  // no layout file needed — we use a fixed layout

    // --- Load Inter font ---
    {
        ImFontConfig font_cfg;
        font_cfg.FontDataOwnedByAtlas = false;  // we own the static array
        font_cfg.OversampleH = 2;
        font_cfg.OversampleV = 1;
        font_cfg.PixelSnapH = true;
        io.Fonts->AddFontFromMemoryTTF(
            (void*)inter_medium_data, inter_medium_size, 15.0f, &font_cfg);
    }

    // --- Theme ---
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();

    // Shape
    style.WindowRounding    = 6.0f;
    style.ChildRounding     = 4.0f;
    style.FrameRounding     = 4.0f;
    style.PopupRounding     = 4.0f;
    style.ScrollbarRounding = 4.0f;
    style.GrabRounding      = 3.0f;
    style.TabRounding       = 4.0f;

    // Spacing
    style.WindowPadding     = ImVec2(14, 14);
    style.FramePadding      = ImVec2(10, 6);
    style.ItemSpacing       = ImVec2(8, 6);
    style.ItemInnerSpacing  = ImVec2(6, 4);
    style.IndentSpacing     = 18.0f;
    style.ScrollbarSize     = 12.0f;
    style.GrabMinSize       = 10.0f;

    // Borders
    style.WindowBorderSize  = 1.0f;
    style.ChildBorderSize   = 0.0f;
    style.FrameBorderSize   = 0.0f;
    style.PopupBorderSize   = 1.0f;
    style.TabBorderSize     = 0.0f;

    // Colors — soft dark theme with teal accent
    ImVec4* c = style.Colors;
    c[ImGuiCol_WindowBg]             = ImVec4(0.10f, 0.10f, 0.11f, 1.00f);
    c[ImGuiCol_ChildBg]              = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_PopupBg]              = ImVec4(0.11f, 0.11f, 0.13f, 0.96f);
    c[ImGuiCol_Border]               = ImVec4(0.22f, 0.22f, 0.24f, 0.60f);
    c[ImGuiCol_BorderShadow]         = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_FrameBg]              = ImVec4(0.16f, 0.16f, 0.18f, 1.00f);
    c[ImGuiCol_FrameBgHovered]       = ImVec4(0.20f, 0.20f, 0.23f, 1.00f);
    c[ImGuiCol_FrameBgActive]        = ImVec4(0.24f, 0.24f, 0.27f, 1.00f);
    c[ImGuiCol_TitleBg]              = ImVec4(0.08f, 0.08f, 0.09f, 1.00f);
    c[ImGuiCol_TitleBgActive]        = ImVec4(0.11f, 0.11f, 0.13f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.08f, 0.08f, 0.09f, 0.75f);
    c[ImGuiCol_MenuBarBg]            = ImVec4(0.12f, 0.12f, 0.14f, 1.00f);
    c[ImGuiCol_ScrollbarBg]          = ImVec4(0.10f, 0.10f, 0.11f, 0.60f);
    c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.28f, 0.28f, 0.30f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.35f, 0.35f, 0.38f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.42f, 0.42f, 0.45f, 1.00f);
    c[ImGuiCol_CheckMark]            = ImVec4(0.40f, 0.78f, 0.80f, 1.00f);
    c[ImGuiCol_SliderGrab]           = ImVec4(0.35f, 0.68f, 0.70f, 1.00f);
    c[ImGuiCol_SliderGrabActive]     = ImVec4(0.45f, 0.82f, 0.85f, 1.00f);
    c[ImGuiCol_Button]               = ImVec4(0.20f, 0.20f, 0.23f, 1.00f);
    c[ImGuiCol_ButtonHovered]        = ImVec4(0.28f, 0.52f, 0.54f, 1.00f);
    c[ImGuiCol_ButtonActive]         = ImVec4(0.32f, 0.62f, 0.64f, 1.00f);
    c[ImGuiCol_Header]               = ImVec4(0.18f, 0.18f, 0.20f, 1.00f);
    c[ImGuiCol_HeaderHovered]        = ImVec4(0.26f, 0.48f, 0.50f, 0.80f);
    c[ImGuiCol_HeaderActive]         = ImVec4(0.30f, 0.58f, 0.60f, 1.00f);
    c[ImGuiCol_Separator]            = ImVec4(0.22f, 0.22f, 0.24f, 0.60f);
    c[ImGuiCol_SeparatorHovered]     = ImVec4(0.35f, 0.68f, 0.70f, 0.80f);
    c[ImGuiCol_SeparatorActive]      = ImVec4(0.40f, 0.78f, 0.80f, 1.00f);
    c[ImGuiCol_ResizeGrip]           = ImVec4(0.35f, 0.68f, 0.70f, 0.25f);
    c[ImGuiCol_ResizeGripHovered]    = ImVec4(0.35f, 0.68f, 0.70f, 0.65f);
    c[ImGuiCol_ResizeGripActive]     = ImVec4(0.40f, 0.78f, 0.80f, 0.90f);
    c[ImGuiCol_TextSelectedBg]       = ImVec4(0.30f, 0.58f, 0.60f, 0.35f);
    c[ImGuiCol_NavHighlight]         = ImVec4(0.40f, 0.78f, 0.80f, 1.00f);

    ImGui_ImplSDL3_InitForSDLRenderer(g_window, g_renderer);
    ImGui_ImplSDLRenderer3_Init(g_renderer);

    g_state.renderer = g_renderer;

    // Enable drag and drop
    SDL_SetEventEnabled(SDL_EVENT_DROP_FILE, true);

    // If launched with a file argument, load it
    if (argc > 1) {
        load_image(g_state, argv[1]);
    }

    return true;
}

static void main_loop_body() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        ImGui_ImplSDL3_ProcessEvent(&event);
        if (event.type == SDL_EVENT_QUIT) {
            g_running = false;
        }
        if (event.type == SDL_EVENT_DROP_FILE) {
            load_image(g_state, event.drop.data);
        }
        if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            g_running = false;
        }
    }

    // Reprocess if parameters changed
    if (g_state.params_dirty && !g_state.pixels_in.empty()) {
        process_image(g_state);
    }

    // Start ImGui frame
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    // --- Fullscreen window, no decorations ---
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##Main", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus);
    ImGui::PopStyleVar(3);

    float full_h = ImGui::GetContentRegionAvail().y;

    // --- Left sidebar (scrollable controls) ---
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 16));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0.10f, 0.11f, 1.0f));
    ImGui::BeginChild("##Sidebar", ImVec2(g_state.sidebar_w, full_h), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    ImGui::Spacing();
    draw_controls(g_state);
    ImGui::EndChild();

    // --- Resize handle between sidebar and preview ---
    ImGui::SameLine(0, 0);
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.14f, 0.14f, 0.15f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  ImVec4(0.28f, 0.52f, 0.54f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,   ImVec4(0.35f, 0.68f, 0.70f, 1.0f));
    ImGui::Button("##Splitter", ImVec2(4.0f, full_h));
    ImGui::PopStyleColor(3);
    if (ImGui::IsItemActive()) {
        g_state.sidebar_w += ImGui::GetIO().MouseDelta.x;
        g_state.sidebar_w = std::clamp(g_state.sidebar_w, 220.0f, 500.0f);
    }
    if (ImGui::IsItemHovered() || ImGui::IsItemActive())
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

    // --- Right preview area ---
    ImGui::SameLine(0, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 16));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.07f, 0.07f, 0.08f, 1.0f));
    ImGui::BeginChild("##Preview", ImVec2(0, full_h), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    draw_preview(g_state);
    ImGui::EndChild();

    ImGui::End();  // ##Main

    // Render
    ImGui::Render();
    SDL_SetRenderDrawColor(g_renderer, 25, 25, 25, 255);
    SDL_RenderClear(g_renderer);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), g_renderer);
    SDL_RenderPresent(g_renderer);
}

int main(int argc, char* argv[]) {
    (void)argc; (void)argv;

    if (!init_app(argc, argv))
        return 1;

#ifdef __EMSCRIPTEN__
    g_wasm_state = &g_state;
    emscripten_set_main_loop(main_loop_body, 0, 1);
#else
    while (g_running) {
        main_loop_body();
    }

    // Cleanup
    if (g_state.tex_in)  SDL_DestroyTexture(g_state.tex_in);
    if (g_state.tex_out) SDL_DestroyTexture(g_state.tex_out);

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    SDL_DestroyRenderer(g_renderer);
    SDL_DestroyWindow(g_window);
    SDL_Quit();
#endif
    return 0;
}
