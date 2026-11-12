// Linux host for the Dear ImGui UI: GLFW 3.4 (native Wayland or X11, picked at runtime) + Vulkan.
// Like the Windows host it renders only when something changes, so it idles at ~0% CPU.
//
// HiDPI: ImGui works in framebuffer pixels on both window systems. On Wayland the compositor gives us a
// framebuffer larger than the logical window (scale 2, or fractional), on X11 the two match and the scale comes
// from Xft.dpi. Either way fonts and sizes are built at the content scale and the mouse is mapped to pixels, so
// text is rasterised at native resolution instead of being upscaled.
#include "app.h"
#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include <stb_image.h>
#include <cstdio>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

int runMcp();

static GLFWwindow *g_window = nullptr;
static VkInstance g_instance = VK_NULL_HANDLE;
static VkPhysicalDevice g_phys = VK_NULL_HANDLE;
static VkDevice g_device = VK_NULL_HANDLE;
static uint32_t g_queueFamily = (uint32_t)-1;
static VkQueue g_queue = VK_NULL_HANDLE;
static VkDescriptorPool g_pool = VK_NULL_HANDLE;
static ImGui_ImplVulkanH_Window g_wd;
static const uint32_t MIN_IMAGES = 2;
static bool g_rebuildSwapchain = false;
static float dpiScale = 1.f;      // UI scale (content scale)
static float fbRatio = 1.f;       // framebuffer pixels per window unit (Wayland scaling)
static bool fontsDirty = false;
static int framesToRender = 3;
static std::atomic<bool> g_focusRequest{ false };

void platformWake() { glfwPostEmptyEvent(); }  // thread-safe

static void fatal(const std::string &msg) {
  fprintf(stderr, "KillerGit: %s\n", msg.c_str());
  try { runProcess("zenity", { "--error", "--title=KillerGit", "--text=" + msg }); } catch (...) {}
  exit(1);
}
static void checkVk(VkResult r) {
  if (r == VK_SUCCESS) return;
  if (r < 0) fatal("Vulkan error " + std::to_string((int)r));
}

// ---------------- single instance ----------------
// An abstract-namespace Unix socket doubles as the instance lock (released automatically when the process dies)
// and as the channel a second launch uses to ask the running window to come forward.
static int g_instanceSock = -1;
static bool claimInstance() {
  sockaddr_un a{}; a.sun_family = AF_UNIX;
  std::string name = "KillerGit.SingleInstance." + std::to_string(getuid());
  memcpy(a.sun_path + 1, name.data(), std::min(name.size(), sizeof a.sun_path - 2));
  socklen_t len = (socklen_t)(offsetof(sockaddr_un, sun_path) + 1 + name.size());
  int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (s < 0) return true;
  if (bind(s, (sockaddr *)&a, len) == 0 && listen(s, 4) == 0) {
    g_instanceSock = s;
    std::thread([s] {
      for (;;) {
        int c = accept(s, nullptr, nullptr);
        if (c < 0) continue;
        close(c);
        g_focusRequest = true; platformWake();
      }
    }).detach();
    return true;
  }
  close(s);
  int c = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (c >= 0) { connect(c, (sockaddr *)&a, len); close(c); }
  return false;
}

// ---------------- Vulkan ----------------
static void setupVulkan() {
  uint32_t extCount = 0;
  const char **glfwExts = glfwGetRequiredInstanceExtensions(&extCount);
  if (!glfwExts) fatal("Vulkan is not available. Install your GPU's Vulkan driver (e.g. mesa-vulkan-drivers).");
  std::vector<const char *> exts(glfwExts, glfwExts + extCount);

  VkApplicationInfo appInfo{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
  appInfo.pApplicationName = "KillerGit"; appInfo.applicationVersion = VK_MAKE_VERSION(0, 2, 0);
  appInfo.pEngineName = "Dear ImGui"; appInfo.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo ci{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
  ci.pApplicationInfo = &appInfo;
  ci.enabledExtensionCount = (uint32_t)exts.size(); ci.ppEnabledExtensionNames = exts.data();
  if (vkCreateInstance(&ci, nullptr, &g_instance) != VK_SUCCESS) fatal("Could not create a Vulkan instance. Is a Vulkan driver installed?");

  // Pick a GPU: discrete > integrated > virtual > CPU (llvmpipe/lavapipe keeps VMs and remote sessions working).
  uint32_t n = 0; vkEnumeratePhysicalDevices(g_instance, &n, nullptr);
  if (!n) fatal("No Vulkan device found.");
  std::vector<VkPhysicalDevice> devs(n); vkEnumeratePhysicalDevices(g_instance, &n, devs.data());
  auto rank = [](VkPhysicalDeviceType t) {
    switch (t) { case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 4; case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 3;
                 case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 2; case VK_PHYSICAL_DEVICE_TYPE_CPU: return 1; default: return 0; }
  };
  int best = -1;
  for (auto d : devs) {
    VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(d, &p);
    if (rank(p.deviceType) > best) { best = rank(p.deviceType); g_phys = d; }
  }
  uint32_t qn = 0; vkGetPhysicalDeviceQueueFamilyProperties(g_phys, &qn, nullptr);
  std::vector<VkQueueFamilyProperties> qs(qn); vkGetPhysicalDeviceQueueFamilyProperties(g_phys, &qn, qs.data());
  for (uint32_t i = 0; i < qn; i++) if (qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { g_queueFamily = i; break; }
  if (g_queueFamily == (uint32_t)-1) fatal("The Vulkan device has no graphics queue.");

  const char *devExts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
  const float prio = 1.f;
  VkDeviceQueueCreateInfo qi{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
  qi.queueFamilyIndex = g_queueFamily; qi.queueCount = 1; qi.pQueuePriorities = &prio;
  VkDeviceCreateInfo di{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
  di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &qi; di.enabledExtensionCount = 1; di.ppEnabledExtensionNames = devExts;
  checkVk(vkCreateDevice(g_phys, &di, nullptr, &g_device));
  vkGetDeviceQueue(g_device, g_queueFamily, 0, &g_queue);

  VkDescriptorPoolSize ps{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16 };
  VkDescriptorPoolCreateInfo pi{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
  pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT; pi.maxSets = 16; pi.poolSizeCount = 1; pi.pPoolSizes = &ps;
  checkVk(vkCreateDescriptorPool(g_device, &pi, nullptr, &g_pool));
}

static void setupWindow(VkSurfaceKHR surface, int w, int h) {
  g_wd.Surface = surface;
  VkBool32 ok = VK_FALSE; vkGetPhysicalDeviceSurfaceSupportKHR(g_phys, g_queueFamily, surface, &ok);
  if (!ok) fatal("The Vulkan device cannot present to this window.");
  const VkFormat fmts[] = { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8_UNORM, VK_FORMAT_R8G8B8_UNORM };
  g_wd.SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(g_phys, surface, fmts, 4, VK_COLORSPACE_SRGB_NONLINEAR_KHR);
  // FIFO = vsync, always available; we only render on demand, so latency/CPU stay low.
  VkPresentModeKHR modes[] = { VK_PRESENT_MODE_FIFO_KHR };
  g_wd.PresentMode = ImGui_ImplVulkanH_SelectPresentMode(g_phys, surface, modes, 1);
  ImGui_ImplVulkanH_CreateOrResizeWindow(g_instance, g_phys, g_device, &g_wd, g_queueFamily, nullptr, w, h, MIN_IMAGES);
}

static void frameRender(ImDrawData *dd) {
  VkSemaphore acquired = g_wd.FrameSemaphores[g_wd.SemaphoreIndex].ImageAcquiredSemaphore;
  VkSemaphore done = g_wd.FrameSemaphores[g_wd.SemaphoreIndex].RenderCompleteSemaphore;
  VkResult err = vkAcquireNextImageKHR(g_device, g_wd.Swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &g_wd.FrameIndex);
  if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) g_rebuildSwapchain = true;
  if (err == VK_ERROR_OUT_OF_DATE_KHR) return;
  if (err != VK_SUBOPTIMAL_KHR) checkVk(err);

  ImGui_ImplVulkanH_Frame *fd = &g_wd.Frames[g_wd.FrameIndex];
  checkVk(vkWaitForFences(g_device, 1, &fd->Fence, VK_TRUE, UINT64_MAX));
  checkVk(vkResetFences(g_device, 1, &fd->Fence));
  checkVk(vkResetCommandPool(g_device, fd->CommandPool, 0));
  VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  checkVk(vkBeginCommandBuffer(fd->CommandBuffer, &bi));
  VkRenderPassBeginInfo rp{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
  rp.renderPass = g_wd.RenderPass; rp.framebuffer = fd->Framebuffer;
  rp.renderArea.extent.width = g_wd.Width; rp.renderArea.extent.height = g_wd.Height;
  rp.clearValueCount = 1; rp.pClearValues = &g_wd.ClearValue;
  vkCmdBeginRenderPass(fd->CommandBuffer, &rp, VK_SUBPASS_CONTENTS_INLINE);
  ImGui_ImplVulkan_RenderDrawData(dd, fd->CommandBuffer);
  vkCmdEndRenderPass(fd->CommandBuffer);
  checkVk(vkEndCommandBuffer(fd->CommandBuffer));
  VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
  si.waitSemaphoreCount = 1; si.pWaitSemaphores = &acquired; si.pWaitDstStageMask = &stage;
  si.commandBufferCount = 1; si.pCommandBuffers = &fd->CommandBuffer;
  si.signalSemaphoreCount = 1; si.pSignalSemaphores = &done;
  checkVk(vkQueueSubmit(g_queue, 1, &si, fd->Fence));
}

static void framePresent() {
  if (g_rebuildSwapchain) return;
  VkSemaphore done = g_wd.FrameSemaphores[g_wd.SemaphoreIndex].RenderCompleteSemaphore;
  VkPresentInfoKHR pi{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
  pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &done;
  pi.swapchainCount = 1; pi.pSwapchains = &g_wd.Swapchain; pi.pImageIndices = &g_wd.FrameIndex;
  VkResult err = vkQueuePresentKHR(g_queue, &pi);
  if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) g_rebuildSwapchain = true;
  if (err == VK_ERROR_OUT_OF_DATE_KHR) return;
  if (err != VK_SUBOPTIMAL_KHR) checkVk(err);
  g_wd.SemaphoreIndex = (g_wd.SemaphoreIndex + 1) % g_wd.SemaphoreCount;
}

// ---------------- icon texture ----------------
struct Texture { VkImage image = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE; VkSampler sampler = VK_NULL_HANDLE; VkDescriptorSet set = VK_NULL_HANDLE; };
static Texture g_icon;

static uint32_t memType(uint32_t bits, VkMemoryPropertyFlags props) {
  VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(g_phys, &mp);
  for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
    if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) return i;
  return UINT32_MAX;
}

static unsigned char *decodeIcon(int &w, int &h) {
  const void *png; size_t size;
  if (!embeddedResource(103, png, size)) return nullptr;
  int comp; return stbi_load_from_memory((const stbi_uc *)png, (int)size, &w, &h, &comp, 4);
}

static ImTextureID loadIconTexture() {
  int w, h; unsigned char *px = decodeIcon(w, h);
  if (!px) return 0;
  VkDeviceSize bytes = (VkDeviceSize)w * h * 4;
  // Staging buffer
  VkBuffer staging; VkDeviceMemory stagingMem;
  VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO }; bci.size = bytes; bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  checkVk(vkCreateBuffer(g_device, &bci, nullptr, &staging));
  VkMemoryRequirements req; vkGetBufferMemoryRequirements(g_device, staging, &req);
  VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO }; ai.allocationSize = req.size;
  ai.memoryTypeIndex = memType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  checkVk(vkAllocateMemory(g_device, &ai, nullptr, &stagingMem));
  vkBindBufferMemory(g_device, staging, stagingMem, 0);
  void *map; vkMapMemory(g_device, stagingMem, 0, bytes, 0, &map); memcpy(map, px, (size_t)bytes); vkUnmapMemory(g_device, stagingMem);
  stbi_image_free(px);
  // Image
  VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
  ici.imageType = VK_IMAGE_TYPE_2D; ici.format = VK_FORMAT_R8G8B8A8_UNORM; ici.extent = { (uint32_t)w, (uint32_t)h, 1 };
  ici.mipLevels = 1; ici.arrayLayers = 1; ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
  ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT; ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  checkVk(vkCreateImage(g_device, &ici, nullptr, &g_icon.image));
  vkGetImageMemoryRequirements(g_device, g_icon.image, &req);
  ai.allocationSize = req.size; ai.memoryTypeIndex = memType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  checkVk(vkAllocateMemory(g_device, &ai, nullptr, &g_icon.mem));
  vkBindImageMemory(g_device, g_icon.image, g_icon.mem, 0);
  VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
  vci.image = g_icon.image; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = VK_FORMAT_R8G8B8A8_UNORM;
  vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
  checkVk(vkCreateImageView(g_device, &vci, nullptr, &g_icon.view));
  VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
  sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR; sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; sci.maxLod = 1.f;
  checkVk(vkCreateSampler(g_device, &sci, nullptr, &g_icon.sampler));
  // Upload with a one-off command buffer on frame 0's pool.
  VkCommandPool pool = g_wd.Frames[0].CommandPool; VkCommandBuffer cb = g_wd.Frames[0].CommandBuffer;
  checkVk(vkResetCommandPool(g_device, pool, 0));
  VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cb, &bi);
  VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = g_icon.image; b.subresourceRange = vci.subresourceRange;
  b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
  VkBufferImageCopy region{}; region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }; region.imageExtent = ici.extent;
  vkCmdCopyBufferToImage(cb, staging, g_icon.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
  vkEndCommandBuffer(cb);
  VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO }; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
  checkVk(vkQueueSubmit(g_queue, 1, &si, VK_NULL_HANDLE));
  checkVk(vkQueueWaitIdle(g_queue));
  vkDestroyBuffer(g_device, staging, nullptr); vkFreeMemory(g_device, stagingMem, nullptr);
  g_icon.set = ImGui_ImplVulkan_AddTexture(g_icon.sampler, g_icon.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  return (ImTextureID)g_icon.set;
}

static void destroyIcon() {
  if (g_icon.set) ImGui_ImplVulkan_RemoveTexture(g_icon.set);
  if (g_icon.sampler) vkDestroySampler(g_device, g_icon.sampler, nullptr);
  if (g_icon.view) vkDestroyImageView(g_device, g_icon.view, nullptr);
  if (g_icon.image) vkDestroyImage(g_device, g_icon.image, nullptr);
  if (g_icon.mem) vkFreeMemory(g_device, g_icon.mem, nullptr);
}

// X11 shows the window icon from _NET_WM_ICON; Wayland ignores this and uses the .desktop file matching our app id.
static void setWindowIcon() {
  if (glfwGetPlatform() != GLFW_PLATFORM_X11) return;
  int w, h; unsigned char *px = decodeIcon(w, h);
  if (!px) return;
  GLFWimage img{ w, h, px }; glfwSetWindowIcon(g_window, 1, &img);
  stbi_image_free(px);
}

// ---------------- input: map window units to framebuffer pixels ----------------
static void updateScale() {
  int ww, wh, fw, fh; glfwGetWindowSize(g_window, &ww, &wh); glfwGetFramebufferSize(g_window, &fw, &fh);
  if (ww > 0) fbRatio = (float)fw / ww;
  float sx, sy; glfwGetWindowContentScale(g_window, &sx, &sy);
  float s = sx > 0 ? sx : 1.f;
  if (std::abs(s - dpiScale) > 0.01f) { dpiScale = s; fontsDirty = true; }
}
static void installCallbacks() {
  glfwSetWindowFocusCallback(g_window, [](GLFWwindow *w, int focused) {
    ImGui_ImplGlfw_WindowFocusCallback(w, focused);
    if (focused && !app.repo.empty() && !app.busy && app.tab == Tab::Changes) app.refreshRepo();
    framesToRender = 3;
  });
  glfwSetCursorEnterCallback(g_window, [](GLFWwindow *w, int e) { ImGui_ImplGlfw_CursorEnterCallback(w, e); framesToRender = 3; });
  glfwSetCursorPosCallback(g_window, [](GLFWwindow *w, double x, double y) { ImGui_ImplGlfw_CursorPosCallback(w, x * fbRatio, y * fbRatio); framesToRender = 3; });
  glfwSetMouseButtonCallback(g_window, [](GLFWwindow *w, int b, int a, int m) { ImGui_ImplGlfw_MouseButtonCallback(w, b, a, m); framesToRender = 3; });
  glfwSetScrollCallback(g_window, [](GLFWwindow *w, double x, double y) { ImGui_ImplGlfw_ScrollCallback(w, x, y); framesToRender = 3; });
  glfwSetKeyCallback(g_window, [](GLFWwindow *w, int k, int sc, int a, int m) { ImGui_ImplGlfw_KeyCallback(w, k, sc, a, m); framesToRender = 3; });
  glfwSetCharCallback(g_window, [](GLFWwindow *w, unsigned c) { ImGui_ImplGlfw_CharCallback(w, c); framesToRender = 3; });
  glfwSetMonitorCallback([](GLFWmonitor *m, int e) { ImGui_ImplGlfw_MonitorCallback(m, e); });
  glfwSetFramebufferSizeCallback(g_window, [](GLFWwindow *, int, int) { g_rebuildSwapchain = true; framesToRender = 3; });
  glfwSetWindowSizeCallback(g_window, [](GLFWwindow *, int, int) { updateScale(); framesToRender = 3; });
  glfwSetWindowContentScaleCallback(g_window, [](GLFWwindow *, float, float) { updateScale(); framesToRender = 3; });
  glfwSetWindowRefreshCallback(g_window, [](GLFWwindow *) { framesToRender = std::max(framesToRender, 1); });
  glfwSetWindowCloseCallback(g_window, [](GLFWwindow *w) {
    int x = 0, y = 0, W, H; glfwGetWindowSize(w, &W, &H);
    json s = { { "width", W }, { "height", H }, { "maximized", glfwGetWindowAttrib(w, GLFW_MAXIMIZED) == GLFW_TRUE } };
    if (glfwGetPlatform() == GLFW_PLATFORM_X11) { glfwGetWindowPos(w, &x, &y); s["winX"] = x; s["winY"] = y; }  // Wayland hides positions
    vault::saveSettings(s);
  });
}

int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--mcp")) return runMcp();
  if (!claimInstance()) return 0;  // the running window was asked to come forward

  // Native Wayland when available, X11 otherwise; KILLERGIT_PLATFORM=x11|wayland forces one.
  const char *forced = getenv("KILLERGIT_PLATFORM");
  if (forced && !strcmp(forced, "x11")) glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
  else if (forced && !strcmp(forced, "wayland")) glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_WAYLAND);
  glfwInitHint(GLFW_WAYLAND_LIBDECOR, GLFW_WAYLAND_PREFER_LIBDECOR);  // proper title bars on GNOME (no server-side decorations)
  glfwSetErrorCallback([](int code, const char *desc) { if (code != GLFW_FEATURE_UNAVAILABLE) fprintf(stderr, "GLFW %d: %s\n", code, desc); });
  if (!glfwInit()) fatal("Could not open a window: neither Wayland nor X11 is available.");
  if (!glfwVulkanSupported()) fatal("Vulkan is not available. Install your GPU's Vulkan driver (e.g. mesa-vulkan-drivers).");

  json s = vault::settings();
  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);    // X11: size the window for the monitor's DPI
  glfwWindowHint(GLFW_SCALE_FRAMEBUFFER, GLFW_TRUE);   // Wayland: full-resolution framebuffer on HiDPI
  glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
  glfwWindowHint(GLFW_MAXIMIZED, s.value("maximized", false) ? GLFW_TRUE : GLFW_FALSE);
  glfwWindowHintString(GLFW_WAYLAND_APP_ID, "killergit");          // matches killergit.desktop (icon, taskbar grouping)
  glfwWindowHintString(GLFW_X11_CLASS_NAME, "killergit");
  glfwWindowHintString(GLFW_X11_INSTANCE_NAME, "killergit");
  g_window = glfwCreateWindow(s.value("width", 1320), s.value("height", 840), "KillerGit", nullptr, nullptr);
  if (!g_window) fatal("Could not create the window.");
  if (glfwGetPlatform() == GLFW_PLATFORM_X11 && s.contains("winX") && s.contains("winY")) {
    int x = s.value("winX", 0), y = s.value("winY", 0), count = 0; bool onScreen = false;
    GLFWmonitor **mons = glfwGetMonitors(&count);
    for (int i = 0; i < count; i++) {  // only restore onto a monitor that is still connected
      int mx, my, mw, mh; glfwGetMonitorWorkarea(mons[i], &mx, &my, &mw, &mh);
      if (x + 40 >= mx && x + 40 < mx + mw && y + 20 >= my && y + 20 < my + mh) onScreen = true;
    }
    if (onScreen) glfwSetWindowPos(g_window, x, y);
  }
  setWindowIcon();

  setupVulkan();
  VkSurfaceKHR surface;
  checkVk(glfwCreateWindowSurface(g_instance, g_window, nullptr, &surface));
  int fw, fh; glfwGetFramebufferSize(g_window, &fw, &fh);
  setupWindow(surface, fw, fh);

  updateScale(); fontsDirty = false;
  int minW = (int)(900 * (fbRatio > 1.01f ? 1.f : dpiScale)), minH = (int)(560 * (fbRatio > 1.01f ? 1.f : dpiScale));
  glfwSetWindowSizeLimits(g_window, minW, minH, GLFW_DONT_CARE, GLFW_DONT_CARE);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.ConfigWindowsMoveFromTitleBarOnly = true;
  ImGui_ImplGlfw_InitForVulkan(g_window, false);
  installCallbacks();
  ImGui_ImplVulkan_InitInfo ii{};
  ii.Instance = g_instance; ii.PhysicalDevice = g_phys; ii.Device = g_device; ii.QueueFamily = g_queueFamily; ii.Queue = g_queue;
  ii.DescriptorPool = g_pool; ii.RenderPass = g_wd.RenderPass; ii.Subpass = 0;
  ii.MinImageCount = MIN_IMAGES; ii.ImageCount = g_wd.ImageCount; ii.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
  ii.CheckVkResultFn = checkVk;
  ImGui_ImplVulkan_Init(&ii);

  buildFonts(dpiScale);
  ImGui_ImplVulkan_CreateFontsTexture();
  g_appIcon = loadIconTexture();
  app.boot();
  glfwShowWindow(g_window);

  while (!glfwWindowShouldClose(g_window)) {
    bool animate = app.busy || app.reposLoading || app.repoLoading || app.diffLoading || app.historyLoading || !app.toasts.empty();
    if (framesToRender <= 0 && !g_dirty) {
      if (animate) glfwWaitEventsTimeout(1.0 / 30); else glfwWaitEvents();
    } else glfwPollEvents();
    framesToRender = std::max(framesToRender, 1);
    if (g_dirty.exchange(false)) framesToRender = std::max(framesToRender, 2);
    if (g_focusRequest.exchange(false)) { glfwShowWindow(g_window); glfwRestoreWindow(g_window); glfwFocusWindow(g_window); glfwRequestWindowAttention(g_window); }
    drainUiQueue();
    if (glfwGetWindowAttrib(g_window, GLFW_ICONIFIED)) { framesToRender = 0; continue; }

    glfwGetFramebufferSize(g_window, &fw, &fh);
    if (fw <= 0 || fh <= 0) { framesToRender = 0; continue; }
    if (g_rebuildSwapchain || fw != g_wd.Width || fh != g_wd.Height) {
      ImGui_ImplVulkan_SetMinImageCount(MIN_IMAGES);
      ImGui_ImplVulkanH_CreateOrResizeWindow(g_instance, g_phys, g_device, &g_wd, g_queueFamily, nullptr, fw, fh, MIN_IMAGES);
      g_wd.FrameIndex = 0; g_rebuildSwapchain = false;
    }
    if (fontsDirty) {
      fontsDirty = false;
      vkDeviceWaitIdle(g_device);
      ImGui_ImplVulkan_DestroyFontsTexture();
      buildFonts(dpiScale);
      ImGui_ImplVulkan_CreateFontsTexture();
      app.saveSettings(json::object());
    }

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    // Work in framebuffer pixels (see top of file).
    io.DisplaySize = ImVec2((float)fw, (float)fh);
    io.DisplayFramebufferScale = ImVec2(1, 1);
    ImGui::NewFrame();
    app.frame();
    ImGui::Render();
    ImDrawData *dd = ImGui::GetDrawData();
    g_wd.ClearValue.color.float32[0] = C.bg.x; g_wd.ClearValue.color.float32[1] = C.bg.y;
    g_wd.ClearValue.color.float32[2] = C.bg.z; g_wd.ClearValue.color.float32[3] = 1.f;
    if (dd->DisplaySize.x > 0 && dd->DisplaySize.y > 0) { frameRender(dd); framePresent(); }
    if (framesToRender > 0) framesToRender--;
    if (animate) framesToRender = std::max(framesToRender, 1);
  }

  vkDeviceWaitIdle(g_device);
  destroyIcon();
  ImGui_ImplVulkan_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  ImGui_ImplVulkanH_DestroyWindow(g_instance, g_device, &g_wd, nullptr);
  vkDestroyDescriptorPool(g_device, g_pool, nullptr);
  vkDestroyDevice(g_device, nullptr);
  vkDestroyInstance(g_instance, nullptr);
  glfwDestroyWindow(g_window);
  glfwTerminate();
  return 0;
}
