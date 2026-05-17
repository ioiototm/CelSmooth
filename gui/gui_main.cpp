#include "celsmooth/mlaa.h"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include <vector>
#include <string>
#include <chrono>
#include <cstdio>
#include <algorithm>

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

    // Layout
    bool first_frame = true;
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

static void draw_controls(AppState& state) {
    ImGui::Begin("Parameters", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

    if (state.img_w > 0) {
        ImGui::Text("Image: %s", state.img_path.c_str());
        ImGui::Text("Size: %d x %d", state.img_w, state.img_h);
        ImGui::Text("Process time: %.1f ms", state.last_process_ms);
        ImGui::Separator();
    }

    ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f), "Core Parameters");
    ImGui::Spacing();

    if (ImGui::SliderFloat("Threshold", &state.params.threshold, 0.01f, 0.5f, "%.3f")) {
        state.params_dirty = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Edge detection sensitivity.\nLower = more edges detected (smoother but may blur detail).\nHigher = fewer edges (preserves detail but less smoothing).");

    int search_dist = state.params.max_distance;
    if (ImGui::SliderInt("Search Distance", &search_dist, 2, 128)) {
        state.params.max_distance = search_dist;
        state.params_dirty = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Maximum edge segment length to process.\nLonger = handles longer straight edges.\nShorter = faster, avoids artifacts on complex shapes.");

    if (ImGui::SliderFloat("Strength", &state.params.strength, 0.0f, 1.5f, "%.2f")) {
        state.params_dirty = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Blend intensity.\n1.0 = full anti-aliasing.\n0.5 = subtle smoothing.\n>1.0 = exaggerated (artistic effect).");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f), "V2 Features");
    ImGui::Spacing();

    bool classic = state.params.classic_mode;
    if (ImGui::Checkbox("Classic Mode (V1 only)", &classic)) {
        state.params.classic_mode = classic;
        state.params_dirty = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Disable all V2 features.\nUses the original 2009 MLAA algorithm only.");

    if (!state.params.classic_mode) {
        ImGui::Indent();

        if (ImGui::Checkbox("T/Cross Shapes", &state.params.enable_t_cross)) {
            state.params_dirty = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Handle T-junctions and cross-shaped intersections.\nImproves smoothing where lines meet at right angles.");

        if (ImGui::Checkbox("Diagonal Detection", &state.params.enable_diagonals)) {
            state.params_dirty = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Detect and smooth 45-degree staircase patterns.\nGreat for diagonal lines in pixel art.");

        if (ImGui::Checkbox("Gamma Correction", &state.params.enable_gamma)) {
            state.params_dirty = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Blend in linear light space (physically correct).\nProduces perceptually even gradients between colors.");

        if (state.params.enable_gamma) {
            if (ImGui::SliderFloat("Extended Gamma", &state.params.extended_gamma, 0.5f, 3.0f, "%.1f")) {
                state.params_dirty = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Boost thin dark strokes.\n1.0 = standard.\n>1.0 = preserves thin lines better (useful for ink outlines).");
        }

        if (ImGui::SliderFloat("Smoothness", &state.params.smoothness, 0.0f, 2.0f, "%.2f")) {
            state.params_dirty = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Controls blend falloff shape.\n1.0 = standard.\n<1.0 = steeper falloff (sharper).\n>1.0 = gentler falloff (smoother).");

        if (ImGui::Checkbox("U-Rounding", &state.params.u_rounding)) {
            state.params_dirty = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Apply w'=2w^2 rounding to U-shaped patterns.\nProduces rounder corners where two edges meet on the same side.");

        ImGui::Unindent();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f), "View");
    ImGui::Spacing();

    ImGui::SliderFloat("Zoom", &state.zoom, 0.25f, 8.0f, "%.2fx");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Scroll wheel also zooms in the preview area.");

    const char* compare_labels[] = { "Toggle (Space)", "Split Wipe" };
    ImGui::Combo("Compare", &state.compare_mode, compare_labels, 2);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Toggle: hold Space to flash the original.\nSplit Wipe: drag a divider line to compare side by side.");

    if (state.compare_mode == 0) {
        ImGui::TextDisabled("Hold SPACE to see original");
    } else {
        ImGui::TextDisabled("Drag the divider line in the preview");
    }

    ImGui::Spacing();
    if (ImGui::Button("Reset Parameters")) {
        state.params = celsmooth::MlaaParams{};
        state.params_dirty = true;
    }

    // --- Save ---
    if (state.img_w > 0) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f), "Output");
        ImGui::Spacing();

        if (ImGui::Button("Save Result")) {
            std::string out_path = make_output_path(state.img_path);
            if (stbi_write_png(out_path.c_str(), state.img_w, state.img_h, 4,
                               state.pixels_out.data(), state.img_w * 4)) {
                state.last_save_path = out_path;
                state.save_flash_timer = 3.0;
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Save the processed image next to the original\nas <filename>_celsmooth.png");

        if (state.save_flash_timer > 0.0) {
            state.save_flash_timer -= ImGui::GetIO().DeltaTime;
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f), "Saved!");
            ImGui::TextWrapped("%s", state.last_save_path.c_str());
        }
    }

    ImGui::End();
}

static void draw_preview(AppState& state) {
    ImGui::Begin("Preview", nullptr,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    if (state.img_w == 0) {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImVec2 text_size = ImGui::CalcTextSize("Drop an image here");
        ImGui::SetCursorPos(ImVec2(
            (avail.x - text_size.x) * 0.5f,
            (avail.y - text_size.y) * 0.5f));
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Drop an image here");
        ImGui::End();
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

    if (!state.tex_in || !state.tex_out) { ImGui::End(); return; }

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

    ImGui::End();
}

int main(int argc, char* argv[]) {
    (void)argc; (void)argv;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow(
        "CelSmooth v0.2.0",
        1280, 800,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) {
        SDL_Log("CreateWindow failed: %s", SDL_GetError());
        return 1;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) {
        SDL_Log("CreateRenderer failed: %s", SDL_GetError());
        return 1;
    }

    // ImGui setup
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding = 3.0f;
    style.GrabRounding = 3.0f;

    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    AppState state;
    state.renderer = renderer;

    // Only force default layout if no imgui.ini exists yet
    // (otherwise ImGui restores the user's saved layout)
    {
        FILE* f = fopen("imgui.ini", "r");
        if (f) {
            fclose(f);
            state.first_frame = false;  // user has a saved layout, don't override
        }
    }

    // Enable drag and drop
    SDL_SetEventEnabled(SDL_EVENT_DROP_FILE, true);

    // If launched with a file argument, load it
    if (argc > 1) {
        load_image(state, argv[1]);
    }

    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            }
            if (event.type == SDL_EVENT_DROP_FILE) {
                load_image(state, event.drop.data);
            }
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                running = false;
            }
        }

        // Reprocess if parameters changed
        if (state.params_dirty && !state.pixels_in.empty()) {
            process_image(state);
        }

        // Start ImGui frame
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        // Dockspace over the whole window
        ImGuiID dockspace_id = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);

        // Set up default layout on first frame (Parameters left, Preview right)
        if (state.first_frame) {
            state.first_frame = false;

            ImGui::DockBuilderRemoveNode(dockspace_id);
            ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dockspace_id, ImGui::GetMainViewport()->Size);

            ImGuiID left_id, right_id;
            ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Left, 0.28f, &left_id, &right_id);

            ImGui::DockBuilderDockWindow("Parameters", left_id);
            ImGui::DockBuilderDockWindow("Preview", right_id);
            ImGui::DockBuilderFinish(dockspace_id);
        }

        draw_controls(state);
        draw_preview(state);

        // Render
        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 25, 25, 25, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    // Cleanup
    if (state.tex_in)  SDL_DestroyTexture(state.tex_in);
    if (state.tex_out) SDL_DestroyTexture(state.tex_out);

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
