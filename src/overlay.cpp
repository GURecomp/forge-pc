// Forge PC menu overlay: Dear ImGui drawn over the game's frame through the mod host's present
// callback (stock imgui_impl_vulkan; Forge on the Switch used its own NVN backend instead).
// Menu and style are Forge's.
// SPDX-License-Identifier: MIT

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>

#include "forge_internal.h"

namespace forge {

const char* const kImGuiVersion = IMGUI_VERSION;

namespace {

constexpr int kDefaultMenuKey = 73; // Insert (USB HID usage / SDL scancode), as in Forge
constexpr uint32_t kRetireFrames = 8; // presents before a replaced framebuffer is destroyed

std::atomic<bool> g_menu_open{false};
std::atomic<bool> g_want_keyboard{false};
std::atomic<bool> g_want_mouse{false};
int g_menu_key{kDefaultMenuKey};
float g_font_size{16.0f};

std::mutex g_input_lock;
std::deque<ModHostInput> g_inputs; // window thread -> present thread

// --- Vulkan state (present thread only) ---

struct VkFns {
    PFN_vkCreateRenderPass CreateRenderPass;
    PFN_vkDestroyRenderPass DestroyRenderPass;
    PFN_vkCreateImageView CreateImageView;
    PFN_vkDestroyImageView DestroyImageView;
    PFN_vkCreateFramebuffer CreateFramebuffer;
    PFN_vkDestroyFramebuffer DestroyFramebuffer;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
    PFN_vkCmdBeginRenderPass CmdBeginRenderPass;
    PFN_vkCmdEndRenderPass CmdEndRenderPass;
    PFN_vkDeviceWaitIdle DeviceWaitIdle;
} vk{};

struct Target {
    VkImageView view{};
    VkFramebuffer framebuffer{};
    uint32_t width{}, height{};
};
struct Retired {
    Target target;
    uint64_t at_frame;
};

bool g_failed{false};
bool g_imgui_ready{false};
bool g_backend_ready{false};
VkInstance g_instance{};
PFN_vkGetInstanceProcAddr g_get_proc{};
VkDevice g_device{};
VkFormat g_format{VK_FORMAT_UNDEFINED};
VkRenderPass g_render_pass{};
uint32_t g_image_count{};
std::unordered_map<uint64_t, Target> g_targets; // by swapchain VkImage
std::vector<Retired> g_retired;
uint64_t g_frame{};
std::string g_ini_path;
LARGE_INTEGER g_last_time{};
std::set<uint32_t> g_plugin_imgui_inited; // PluginUi::generation (new on every hot reload)

PFN_vkVoidFunction LoadVk(const char* name, void*) {
    return g_get_proc(g_instance, name);
}

void DestroyTarget(const Target& t) {
    if (t.framebuffer) {
        vk.DestroyFramebuffer(g_device, t.framebuffer, nullptr);
    }
    if (t.view) {
        vk.DestroyImageView(g_device, t.view, nullptr);
    }
}

void CheckVk(VkResult r) {
    if (r != VK_SUCCESS) {
        Log(FORGE_LOG_ERROR, "Vulkan error %d in the menu overlay", static_cast<int>(r));
    }
}

// --- style (Forge's) ---

void SetupStyle() {
    auto& style = ImGui::GetStyle();
    style.WindowPadding = {12.0f, 12.0f};
    style.WindowRounding = 2.0f;
    style.WindowBorderSize = 1.0f;
    style.WindowMinSize = {20.0f, 20.0f};
    style.WindowTitleAlign = {0.5f, 0.5f};
    style.WindowMenuButtonPosition = ImGuiDir_None;
    style.ChildRounding = 0.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupRounding = 0.0f;
    style.PopupBorderSize = 1.0f;
    style.FramePadding = {6.0f, 6.0f};
    style.FrameRounding = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.ItemSpacing = {12.0f, 6.0f};
    style.ItemInnerSpacing = {6.0f, 3.0f};
    style.CellPadding = {12.0f, 6.0f};
    style.IndentSpacing = 20.0f;
    style.ColumnsMinSpacing = 6.0f;
    style.ScrollbarSize = 12.0f;
    style.ScrollbarRounding = 0.0f;
    style.GrabMinSize = 12.0f;
    style.GrabRounding = 1.0f;
    style.TabRounding = 0.0f;
    style.TabBorderSize = 0.0f;
    style.TabCloseButtonMinWidthUnselected = 0.0f;
    style.ColorButtonPosition = ImGuiDir_Right;
    style.ButtonTextAlign = {0.5f, 0.5f};
    style.SelectableTextAlign = {0.0f, 0.0f};

    auto* c = style.Colors;
    const ImVec4 bg{0.0784f, 0.0863f, 0.1020f, 1.0f};
    const ImVec4 bg_dark{0.0471f, 0.0549f, 0.0706f, 1.0f};
    const ImVec4 bg_mid{0.0980f, 0.1059f, 0.1216f, 1.0f};
    const ImVec4 frame{0.1176f, 0.1333f, 0.1490f, 1.0f};
    const ImVec4 frame_hover{0.1569f, 0.1686f, 0.1922f, 1.0f};
    const ImVec4 accent{0.4980f, 0.5137f, 1.0f, 1.0f};
    const ImVec4 hover{0.1961f, 0.1765f, 0.5451f, 1.0f};
    const ImVec4 active{0.2353f, 0.2157f, 0.5961f, 1.0f};
    const ImVec4 sep{0.1569f, 0.1843f, 0.2510f, 1.0f};
    c[ImGuiCol_Text] = {1.0f, 1.0f, 1.0f, 1.0f};
    c[ImGuiCol_TextDisabled] = {0.2745f, 0.3176f, 0.4510f, 1.0f};
    c[ImGuiCol_WindowBg] = c[ImGuiCol_ChildBg] = c[ImGuiCol_PopupBg] = bg;
    c[ImGuiCol_Border] = frame_hover;
    c[ImGuiCol_BorderShadow] = bg;
    c[ImGuiCol_FrameBg] = frame;
    c[ImGuiCol_FrameBgHovered] = frame_hover;
    c[ImGuiCol_FrameBgActive] = active;
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = bg_dark;
    c[ImGuiCol_TitleBgCollapsed] = bg;
    c[ImGuiCol_MenuBarBg] = bg_mid;
    c[ImGuiCol_ScrollbarBg] = bg_dark;
    c[ImGuiCol_ScrollbarGrab] = c[ImGuiCol_ScrollbarGrabActive] = frame;
    c[ImGuiCol_ScrollbarGrabHovered] = frame_hover;
    c[ImGuiCol_CheckMark] = c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = {0.5373f, 0.5529f, 1.0f, 1.0f};
    c[ImGuiCol_Button] = {0.257f, 0.267f, 0.554f, 1.0f};
    c[ImGuiCol_ButtonHovered] = c[ImGuiCol_HeaderHovered] = c[ImGuiCol_ResizeGripHovered] = hover;
    c[ImGuiCol_ButtonActive] = c[ImGuiCol_HeaderActive] = c[ImGuiCol_ResizeGripActive] = active;
    c[ImGuiCol_Header] = c[ImGuiCol_ResizeGrip] = frame;
    c[ImGuiCol_Separator] = c[ImGuiCol_SeparatorHovered] = c[ImGuiCol_SeparatorActive] = sep;
    c[ImGuiCol_Tab] = c[ImGuiCol_TabUnfocused] = bg_dark;
    c[ImGuiCol_TabHovered] = frame;
    c[ImGuiCol_TabActive] = bg_mid;
    c[ImGuiCol_TabUnfocusedActive] = bg;
    c[ImGuiCol_PlotLines] = {0.5216f, 0.6000f, 0.7020f, 1.0f};
    c[ImGuiCol_PlotLinesHovered] = {0.0392f, 0.9804f, 0.9804f, 1.0f};
    c[ImGuiCol_PlotHistogram] = {1.0f, 0.2902f, 0.5961f, 1.0f};
    c[ImGuiCol_PlotHistogramHovered] = {0.9961f, 0.4745f, 0.6980f, 1.0f};
    c[ImGuiCol_TableHeaderBg] = c[ImGuiCol_TableBorderStrong] = bg_dark;
    c[ImGuiCol_TableBorderLight] = {0.0f, 0.0f, 0.0f, 1.0f};
    c[ImGuiCol_TableRowBg] = frame;
    c[ImGuiCol_TableRowBgAlt] = bg_mid;
    c[ImGuiCol_TextSelectedBg] = active;
    c[ImGuiCol_DragDropTarget] = c[ImGuiCol_NavCursor] = c[ImGuiCol_NavWindowingHighlight] =
        accent;
    c[ImGuiCol_NavWindowingDimBg] = c[ImGuiCol_ModalWindowDimBg] = {0.1961f, 0.1765f, 0.5451f,
                                                                       0.5020f};
}

// --- input ---

ImGuiKey KeyFromScancode(int sc) {
    if (sc >= 4 && sc <= 29) {
        return static_cast<ImGuiKey>(ImGuiKey_A + (sc - 4));
    }
    if (sc >= 30 && sc <= 38) {
        return static_cast<ImGuiKey>(ImGuiKey_1 + (sc - 30));
    }
    if (sc >= 58 && sc <= 69) {
        return static_cast<ImGuiKey>(ImGuiKey_F1 + (sc - 58));
    }
    if (sc >= 89 && sc <= 97) {
        return static_cast<ImGuiKey>(ImGuiKey_Keypad1 + (sc - 89));
    }
    switch (sc) {
    case 39: return ImGuiKey_0;
    case 40: return ImGuiKey_Enter;
    case 41: return ImGuiKey_Escape;
    case 42: return ImGuiKey_Backspace;
    case 43: return ImGuiKey_Tab;
    case 44: return ImGuiKey_Space;
    case 45: return ImGuiKey_Minus;
    case 46: return ImGuiKey_Equal;
    case 47: return ImGuiKey_LeftBracket;
    case 48: return ImGuiKey_RightBracket;
    case 49: return ImGuiKey_Backslash;
    case 51: return ImGuiKey_Semicolon;
    case 52: return ImGuiKey_Apostrophe;
    case 53: return ImGuiKey_GraveAccent;
    case 54: return ImGuiKey_Comma;
    case 55: return ImGuiKey_Period;
    case 56: return ImGuiKey_Slash;
    case 57: return ImGuiKey_CapsLock;
    case 73: return ImGuiKey_Insert;
    case 74: return ImGuiKey_Home;
    case 75: return ImGuiKey_PageUp;
    case 76: return ImGuiKey_Delete;
    case 77: return ImGuiKey_End;
    case 78: return ImGuiKey_PageDown;
    case 79: return ImGuiKey_RightArrow;
    case 80: return ImGuiKey_LeftArrow;
    case 81: return ImGuiKey_DownArrow;
    case 82: return ImGuiKey_UpArrow;
    case 88: return ImGuiKey_KeypadEnter;
    case 98: return ImGuiKey_Keypad0;
    case 224: return ImGuiKey_LeftCtrl;
    case 225: return ImGuiKey_LeftShift;
    case 226: return ImGuiKey_LeftAlt;
    case 227: return ImGuiKey_LeftSuper;
    case 228: return ImGuiKey_RightCtrl;
    case 229: return ImGuiKey_RightShift;
    case 230: return ImGuiKey_RightAlt;
    case 231: return ImGuiKey_RightSuper;
    default: return ImGuiKey_None;
    }
}

void FeedInputs() {
    std::deque<ModHostInput> inputs;
    {
        std::scoped_lock lk{g_input_lock};
        inputs.swap(g_inputs);
    }
    ImGuiIO& io = ImGui::GetIO();
    for (const auto& e : inputs) {
        switch (e.type) {
        case MODHOST_INPUT_KEY: {
            io.AddKeyEvent(ImGuiMod_Shift, (e.mods & 1) != 0);
            io.AddKeyEvent(ImGuiMod_Ctrl, (e.mods & 2) != 0);
            io.AddKeyEvent(ImGuiMod_Alt, (e.mods & 4) != 0);
            io.AddKeyEvent(ImGuiMod_Super, (e.mods & 8) != 0);
            if (const ImGuiKey key = KeyFromScancode(e.scancode); key != ImGuiKey_None) {
                io.AddKeyEvent(key, e.down);
            }
            break;
        }
        case MODHOST_INPUT_MOUSE_MOVE:
            io.AddMousePosEvent(e.x, e.y);
            break;
        case MODHOST_INPUT_MOUSE_BUTTON: {
            static constexpr int kMap[] = {-1, 0, 2, 1, 3, 4}; // left, middle, right, x1, x2
            if (e.button >= 1 && e.button <= 5) {
                io.AddMousePosEvent(e.x, e.y);
                io.AddMouseButtonEvent(kMap[e.button], e.down);
            }
            break;
        }
        case MODHOST_INPUT_MOUSE_WHEEL:
            io.AddMouseWheelEvent(e.wheel_x, e.wheel_y);
            break;
        case MODHOST_INPUT_TEXT:
            io.AddInputCharactersUTF8(e.text);
            break;
        case MODHOST_INPUT_FOCUS:
            io.AddFocusEvent(e.down);
            break;
        }
    }
}

// --- Vulkan setup ---

bool CreateRenderPass() {
    const VkAttachmentDescription color{
        .format = g_format,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, // keep the game's frame underneath
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };
    const VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    const VkSubpassDescription subpass{
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &ref,
    };
    const VkSubpassDependency dep{
        .srcSubpass = VK_SUBPASS_EXTERNAL,
        .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = 0,
        .dstAccessMask =
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
    };
    const VkRenderPassCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &color,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 1,
        .pDependencies = &dep,
    };
    return vk.CreateRenderPass(g_device, &info, nullptr, &g_render_pass) == VK_SUCCESS;
}

bool InitBackend(const ModHostPresent* p) {
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = p->vk_api_version;
    info.Instance = g_instance;
    info.PhysicalDevice = static_cast<VkPhysicalDevice>(p->vk_physical_device);
    info.Device = g_device;
    info.QueueFamily = p->vk_queue_family;
    info.Queue = static_cast<VkQueue>(p->vk_queue);
    info.DescriptorPoolSize = 64; // the backend makes its own pool
    info.MinImageCount = std::max<uint32_t>(2, p->image_count);
    info.ImageCount = std::max<uint32_t>(2, p->image_count);
    info.PipelineInfoMain.RenderPass = g_render_pass;
    info.PipelineInfoMain.Subpass = 0;
    info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    info.CheckVkResultFn = CheckVk;
    return ImGui_ImplVulkan_Init(&info);
}

bool InitImGui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    g_ini_path = LoaderDir() + "/imgui.ini";
    io.IniFilename = g_ini_path.c_str();
    io.BackendPlatformName = "forge_pc";
    ImGui::StyleColorsDark();
    SetupStyle();
    ImGui::GetStyle().FontSizeBase = g_font_size;
    const std::string font = LoaderDir() + "/fonts/Roboto-Medium.ttf";
    std::error_code ec;
    if (std::filesystem::is_regular_file(PathU8(font), ec)) {
        io.Fonts->AddFontFromFileTTF(font.c_str(), g_font_size);
    } else {
        io.Fonts->AddFontDefault();
    }
    QueryPerformanceCounter(&g_last_time);
    g_imgui_ready = true;
    return true;
}

/// Framebuffer for this swapchain image (made on first use; replaced when its size changes).
Target* GetTarget(const ModHostPresent* p) {
    auto& t = g_targets[p->vk_image];
    if (t.framebuffer && (t.width != p->width || t.height != p->height)) {
        g_retired.push_back({t, g_frame});
        t = {};
    }
    if (!t.framebuffer) {
        const VkImageViewCreateInfo view_info{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = reinterpret_cast<VkImage>(p->vk_image),
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = g_format,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        if (vk.CreateImageView(g_device, &view_info, nullptr, &t.view) != VK_SUCCESS) {
            return nullptr;
        }
        const VkFramebufferCreateInfo fb_info{
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = g_render_pass,
            .attachmentCount = 1,
            .pAttachments = &t.view,
            .width = p->width,
            .height = p->height,
            .layers = 1,
        };
        if (vk.CreateFramebuffer(g_device, &fb_info, nullptr, &t.framebuffer) != VK_SUCCESS) {
            vk.DestroyImageView(g_device, t.view, nullptr);
            t = {};
            return nullptr;
        }
        t.width = p->width;
        t.height = p->height;
    }
    // A recreated swapchain leaves old images behind: retire entries beyond a few sets.
    if (g_targets.size() > std::max<size_t>(8, p->image_count * 3)) {
        for (auto it = g_targets.begin(); it != g_targets.end();) {
            if (it->first != p->vk_image) {
                g_retired.push_back({it->second, g_frame});
                it = g_targets.erase(it);
            } else {
                ++it;
            }
        }
        return &g_targets[p->vk_image];
    }
    return &t;
}

void DestroyRetired(bool all) {
    std::erase_if(g_retired, [all](const Retired& r) {
        if (all || g_frame - r.at_frame >= kRetireFrames) {
            DestroyTarget(r.target);
            return true;
        }
        return false;
    });
}

bool SetupVulkan(const ModHostPresent* p) {
    const auto format = static_cast<VkFormat>(p->vk_format);
    if (g_backend_ready && format == g_format && g_device == p->vk_device) {
        if (p->image_count != g_image_count && p->image_count >= 2) {
            g_image_count = p->image_count;
            ImGui_ImplVulkan_SetMinImageCount(p->image_count);
        }
        return true;
    }
    if (g_backend_ready) {
        // New swapchain format (or device): rebuild the backend and everything that depends
        // on the render pass, with the GPU idle.
        p->lock_queue();
        vk.DeviceWaitIdle(g_device);
        p->unlock_queue();
        ImGui_ImplVulkan_Shutdown();
        g_backend_ready = false;
        for (auto& [image, t] : g_targets) {
            DestroyTarget(t);
        }
        g_targets.clear();
        DestroyRetired(true);
        vk.DestroyRenderPass(g_device, g_render_pass, nullptr);
        g_render_pass = {};
    }
    g_instance = static_cast<VkInstance>(p->vk_instance);
    g_get_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(p->vk_get_instance_proc);
    g_device = static_cast<VkDevice>(p->vk_device);
    g_format = format;
    g_image_count = p->image_count;
#define LOAD(name) vk.name = reinterpret_cast<PFN_vk##name>(g_get_proc(g_instance, "vk" #name))
    LOAD(CreateRenderPass);
    LOAD(DestroyRenderPass);
    LOAD(CreateImageView);
    LOAD(DestroyImageView);
    LOAD(CreateFramebuffer);
    LOAD(DestroyFramebuffer);
    LOAD(CmdPipelineBarrier);
    LOAD(CmdBeginRenderPass);
    LOAD(CmdEndRenderPass);
    LOAD(DeviceWaitIdle);
#undef LOAD
    if (!vk.CreateRenderPass || !vk.CmdBeginRenderPass || !vk.DeviceWaitIdle) {
        return false;
    }
    if (!ImGui_ImplVulkan_LoadFunctions(p->vk_api_version, LoadVk)) {
        Log(FORGE_LOG_ERROR, "menu overlay: could not load Vulkan functions");
        return false;
    }
    if (!CreateRenderPass() || !InitBackend(p)) {
        Log(FORGE_LOG_ERROR, "menu overlay: Vulkan setup failed");
        return false;
    }
    g_backend_ready = true;
    Log(FORGE_LOG_INFO, "menu overlay ready (%ux%u, format %u, %u images); menu key %d",
        p->width, p->height, p->vk_format, p->image_count, g_menu_key);
    return true;
}

// --- the menu (Forge's) ---

void DrawMenu() {
    bool open = g_menu_open.load();
    if (open) {
        ImGui::SetNextWindowSize({460.0f, 360.0f}, ImGuiCond_FirstUseEver);
        if (ImGui::Begin("forge", &open)) {
            if (ImGui::CollapsingHeader("About Forge")) {
                ImGui::Text("Forge PC %d.%d.%d", FORGE_PC_VERSION_MAJOR, FORGE_PC_VERSION_MINOR,
                            FORGE_PC_VERSION_PATCH);
                ImGui::SeparatorText("Forge (Switch) by");
                ImGui::BulletText("Fexty");
                ImGui::BulletText("jeffi2287");
                ImGui::SeparatorText("Libraries Used");
                ImGui::BulletText("Dear ImGui %s", IMGUI_VERSION);
                ImGui::BulletText("Roboto font (Apache License 2.0)");
                ImGui::TextDisabled("Licenses: Forge/licenses/");
            }
            if (ImGui::CollapsingHeader("Loaded Plugins", ImGuiTreeNodeFlags_DefaultOpen)) {
                if (PluginCount() == 0) {
                    ImGui::TextDisabled("none (mods/<title id>/<mod>/plugins/*.dll)");
                }
                std::vector<size_t> reload;
                ForEachPluginUi([&](size_t i, const PluginUi& ui) {
                    ImGui::PushID(static_cast<int>(i));
                    ImGui::BulletText("%s", ui.name.c_str());
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Reload")) {
                        reload.push_back(i); // outside the plugin lock
                    }
                    ImGui::PopID();
                });
                for (const size_t i : reload) {
                    RequestPluginReload(i);
                }
                ImGui::TextDisabled(HotReloadOn() ? "hot reload on: rebuilt plugins reload by "
                                                    "themselves"
                                                  : "hot reload off ([plugins] hot_reload)");
            }
            if (ImGui::CollapsingHeader("Plugin UI")) {
                ForEachPluginUi([](size_t, const PluginUi& ui) {
                    if (ui.on_imgui_render) {
                        ui.on_imgui_render();
                    }
                });
            }
        }
        ImGui::End();
        if (!open) {
            g_menu_open = false;
            g_host->set_input_captured(false);
        }
    }
    ForEachPluginUi([](size_t, const PluginUi& ui) {
        if (ui.on_imgui_free_render) {
            ui.on_imgui_free_render();
        }
    });
}

void InitPluginImGui() {
    ImGuiMemAllocFunc alloc{};
    ImGuiMemFreeFunc free_fn{};
    void* user{};
    ImGui::GetAllocatorFunctions(&alloc, &free_fn, &user);
    ForEachPluginUi([&](size_t, const PluginUi& ui) {
        if (g_plugin_imgui_inited.insert(ui.generation).second) {
            if (ui.on_imgui_init) {
                ui.on_imgui_init(ImGui::GetCurrentContext(), reinterpret_cast<void*>(alloc),
                                 reinterpret_cast<void*>(free_fn), user);
            }
        }
    });
}

bool AnyFreeRender() {
    bool any = false;
    ForEachPluginUi([&](size_t, const PluginUi& ui) { any |= ui.on_imgui_free_render != nullptr; });
    return any;
}

} // namespace

bool MenuIsOpen() {
    return g_menu_open.load(std::memory_order_relaxed);
}

bool OnInput(const ModHostInput* e) {
    if (e->type == MODHOST_INPUT_KEY && e->scancode == g_menu_key) {
        if (e->down && !e->repeat) {
            const bool open = !g_menu_open.load();
            g_menu_open = open;
            g_host->set_input_captured(open); // the pad drives the menu, not the hunter
        }
        return true; // the toggle key never reaches the game
    }
    const bool open = g_menu_open.load(std::memory_order_relaxed);
    if (!open && !g_want_mouse.load() && !g_want_keyboard.load()) {
        if (e->type == MODHOST_INPUT_MOUSE_MOVE || e->type == MODHOST_INPUT_FOCUS) {
            std::scoped_lock lk{g_input_lock}; // keeps plugin windows' hover right
            if (g_inputs.size() < 256) {
                g_inputs.push_back(*e);
            }
        }
        return false;
    }
    {
        std::scoped_lock lk{g_input_lock};
        if (g_inputs.size() < 1024) {
            g_inputs.push_back(*e);
        }
    }
    // Releases always reach the game too, so nothing stays held from before the menu opened.
    const bool release = (e->type == MODHOST_INPUT_KEY || e->type == MODHOST_INPUT_MOUSE_BUTTON) &&
                         !e->down;
    return !release && e->type != MODHOST_INPUT_FOCUS;
}

bool OnPresent(const ModHostPresent* p) {
    if (g_failed || !MODHOST_HAS(ModHostPresent, p, unlock_queue)) {
        return false;
    }
    ++g_frame;
    // Nothing to draw: no work at all (the game looks exactly as without Forge).
    if (!g_menu_open.load() && !AnyFreeRender()) {
        return false;
    }
    if (!g_imgui_ready && !InitImGui()) {
        g_failed = true;
        return false;
    }
    if (!SetupVulkan(p)) {
        g_failed = true;
        return false;
    }
    DestroyRetired(false);
    Target* target = GetTarget(p);
    if (!target) {
        return false;
    }

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = {static_cast<float>(p->width), static_cast<float>(p->height)};
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    io.DeltaTime = std::max(1e-4f, static_cast<float>(now.QuadPart - g_last_time.QuadPart) /
                                       static_cast<float>(freq.QuadPart));
    g_last_time = now;
    io.MouseDrawCursor = g_menu_open.load();
    FeedInputs();
    InitPluginImGui();

    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    DrawMenu();
    ImGui::Render();
    g_want_keyboard = io.WantCaptureKeyboard && g_menu_open.load();
    g_want_mouse = io.WantCaptureMouse && g_menu_open.load();
    g_host->set_text_input(io.WantTextInput);

    ImDrawData* dd = ImGui::GetDrawData();
    if (!dd || dd->CmdListsCount == 0) {
        return false;
    }
    const auto cmd = static_cast<VkCommandBuffer>(p->vk_command_buffer);
    const VkImageMemoryBarrier to_color{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = reinterpret_cast<VkImage>(p->vk_image),
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr,
                          1, &to_color);
    const VkRenderPassBeginInfo begin{
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = g_render_pass,
        .framebuffer = target->framebuffer,
        .renderArea = {{0, 0}, {p->width, p->height}},
    };
    vk.CmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
    // Texture uploads (the font atlas) submit to the shared queue inside RenderDrawData.
    p->lock_queue();
    ImGui_ImplVulkan_RenderDrawData(dd, cmd);
    p->unlock_queue();
    vk.CmdEndRenderPass(cmd);
    return true;
}

void OverlayConfigure() {
    if (const std::string key = ConfigValue("menu", "key"); !key.empty()) {
        g_menu_key = std::atoi(key.c_str());
    }
    if (const std::string size = ConfigValue("menu", "font_size"); !size.empty()) {
        g_font_size = std::clamp(static_cast<float>(std::atof(size.c_str())), 8.0f, 64.0f);
    }
}

void OverlayShutdown() {
    // At exit the renderer may already be tearing down its device: stop drawing and leave the
    // few Vulkan objects to the process exit instead of destroying them from this thread.
    g_failed = true;
    if (g_menu_open.exchange(false)) {
        g_host->set_input_captured(false);
    }
}

} // namespace forge
