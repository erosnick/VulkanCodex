#include <windows.h>
#include <vulkan/vulkan.h>
#include <shaderc/shaderc.hpp>
#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
constexpr wchar_t WindowName[] = L"Vulkan Triangle";
constexpr uint32_t WindowWidth = 800;
constexpr uint32_t WindowHeight = 600;
constexpr uint32_t VertexCount = 3;
constexpr uint32_t ColorChannels = 3;
constexpr uint64_t InfiniteTimeout = std::numeric_limits<uint64_t>::max();
constexpr char ValidationLayer[] = "VK_LAYER_KHRONOS_validation";
constexpr char ShaderEntry[] = "main";
using TriangleData = std::array<glm::vec4, VertexCount * 2>;
const glm::vec4 Cornflower(100.0f / 255.0f, 149.0f / 255.0f, 237.0f / 255.0f, 1.0f);

// 着色器内嵌，保持单一源文件；顶点数据由 GLM 提供。
constexpr char VertexShader[] = R"glsl(
#version 450
const int VertexCount = 3;
layout(push_constant) uniform Triangle {
    vec4 positions[VertexCount];
    vec4 colors[VertexCount];
} triangle;
layout(location = 0) out vec3 color;
void main() {
    gl_Position = triangle.positions[gl_VertexIndex];
    color = triangle.colors[gl_VertexIndex].rgb;
}
)glsl";

constexpr char FragmentShader[] = R"glsl(
#version 450
layout(location = 0) in vec3 color;
layout(location = 0) out vec4 outColor;
void main() {
    outColor = vec4(color, 1.0);
}
)glsl";

void check(VkResult result, const char* operation)
{
    if (result != VK_SUCCESS)
    {
        throw std::runtime_error(std::string(operation) + ": " + std::to_string(result));
    }
}

VKAPI_ATTR VkBool32 VKAPI_CALL debugLog(
    VkDebugUtilsMessageSeverityFlagBitsEXT,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void*)
{
    OutputDebugStringA(data->pMessage);
    OutputDebugStringA("\n");
    std::fprintf(stderr, "%s\n", data->pMessage);
    return VK_FALSE;
}
}

class Triangle
{
    friend int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int);

    HINSTANCE module = GetModuleHandleW(nullptr);
    HWND window = nullptr;
    bool resized = false;
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkQueue queue = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    std::vector<VkFramebuffer> frames;
    std::vector<VkSemaphore> presented;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    static LRESULT CALLBACK windowProc(HWND handle, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto* app = reinterpret_cast<Triangle*>(GetWindowLongPtrW(handle, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            app = static_cast<Triangle*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            SetWindowLongPtrW(handle, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }

        if (message == WM_SIZE && app)
        {
            app->resized = true;
        }

        if (message == WM_CLOSE || (message == WM_KEYDOWN && wParam == VK_ESCAPE))
        {
            PostQuitMessage(0);
            return 0;
        }

        return DefWindowProcW(handle, message, wParam, lParam);
    }

    void createWindow()
    {
        WNDCLASSW info{};
        info.lpfnWndProc = windowProc;
        info.hInstance = module;
        info.lpszClassName = WindowName;
        info.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (!RegisterClassW(&info))
        {
            throw std::runtime_error("RegisterClassW failed");
        }

        constexpr DWORD WindowStyle = WS_OVERLAPPEDWINDOW;
        RECT bounds{0, 0, WindowWidth, WindowHeight};
        AdjustWindowRect(&bounds, WindowStyle, FALSE);
        window = CreateWindowW(WindowName, WindowName, WindowStyle,
            CW_USEDEFAULT, CW_USEDEFAULT, bounds.right - bounds.left,
            bounds.bottom - bounds.top, nullptr, nullptr, module, this);
        if (!window)
        {
            throw std::runtime_error("CreateWindowW failed");
        }
    }

    void createInstance()
    {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "Vulkan Triangle";
        app.apiVersion = VK_API_VERSION_1_0;

        std::vector<const char*> extensions{VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
        VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        info.pApplicationInfo = &app;
        VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
#ifdef _DEBUG
        // 有验证层时启用，便于定位 API 使用错误。
        uint32_t count = 0;
        check(vkEnumerateInstanceLayerProperties(&count, nullptr), "List layers");
        std::vector<VkLayerProperties> layers(count);
        check(vkEnumerateInstanceLayerProperties(&count, layers.data()), "List layers");
        const bool validation = std::any_of(layers.begin(), layers.end(), [](const auto& layer)
        {
            return std::strcmp(layer.layerName, ValidationLayer) == 0;
        });
        const char* layer = ValidationLayer;
        if (validation)
        {
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            info.enabledLayerCount = 1;
            info.ppEnabledLayerNames = &layer;
            debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            debug.pfnUserCallback = debugLog;
            info.pNext = &debug;
        }
#endif
        info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        info.ppEnabledExtensionNames = extensions.data();
        check(vkCreateInstance(&info, nullptr, &instance), "Create instance");
        if (info.enabledLayerCount)
        {
            auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
            if (!create)
            {
                throw std::runtime_error("Debug messenger unavailable");
            }
            check(create(instance, &debug, nullptr, &messenger), "Create debug messenger");
        }

        VkWin32SurfaceCreateInfoKHR surfaceInfo{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        surfaceInfo.hinstance = module;
        surfaceInfo.hwnd = window;
        check(vkCreateWin32SurfaceKHR(instance, &surfaceInfo, nullptr, &surface), "Create surface");
    }

    bool selectGpu(VkPhysicalDevice candidate)
    {
        uint32_t count = 0;
        check(vkEnumerateDeviceExtensionProperties(candidate, nullptr, &count, nullptr), "List extensions");
        std::vector<VkExtensionProperties> extensions(count);
        check(vkEnumerateDeviceExtensionProperties(candidate, nullptr, &count, extensions.data()), "List extensions");
        if (std::none_of(extensions.begin(), extensions.end(), [](const auto& extension)
        {
            return std::strcmp(extension.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0;
        }))
        {
            return false;
        }

        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, families.data());
        for (uint32_t index = 0; index < count; ++index)
        {
            VkBool32 supportsPresent = VK_FALSE;
            check(vkGetPhysicalDeviceSurfaceSupportKHR(candidate, index, surface, &supportsPresent), "Check presentation");
            if (!(families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !supportsPresent)
            {
                continue;
            }

            gpu = candidate;
            queueFamily = index;
            return true;
        }
        return false;
    }

    void createDevice()
    {
        uint32_t count = 0;
        check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "List GPUs");
        std::vector<VkPhysicalDevice> candidates(count);
        check(vkEnumeratePhysicalDevices(instance, &count, candidates.data()), "List GPUs");
        for (auto candidate : candidates)
        {
            if (selectGpu(candidate))
            {
                break;
            }
        }
        if (!gpu)
        {
            throw std::runtime_error("No Vulkan GPU with graphics and presentation support");
        }

        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = queueFamily;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;
        const char* extension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        info.queueCreateInfoCount = 1;
        info.pQueueCreateInfos = &queueInfo;
        info.enabledExtensionCount = 1;
        info.ppEnabledExtensionNames = &extension;
        check(vkCreateDevice(gpu, &info, nullptr, &device), "Create device");
        vkGetDeviceQueue(device, queueFamily, 0, &queue);
    }

    VkShaderModule compile(const char* source, shaderc_shader_kind kind)
    {
        shaderc::Compiler compiler;
        auto result = compiler.CompileGlslToSpv(source, kind, "main.cpp");
        if (result.GetCompilationStatus() != shaderc_compilation_status_success)
        {
            throw std::runtime_error(result.GetErrorMessage());
        }

        const std::vector<uint32_t> code(result.cbegin(), result.cend());
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = code.size() * sizeof(uint32_t);
        info.pCode = code.data();
        VkShaderModule shader = VK_NULL_HANDLE;
        check(vkCreateShaderModule(device, &info, nullptr, &shader), "Create shader");
        return shader;
    }

    void createPipeline()
    {
        VkAttachmentDescription attachment{};
        attachment.format = format;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &color;
        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo passInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        passInfo.attachmentCount = 1;
        passInfo.pAttachments = &attachment;
        passInfo.subpassCount = 1;
        passInfo.pSubpasses = &subpass;
        passInfo.dependencyCount = 1;
        passInfo.pDependencies = &dependency;
        check(vkCreateRenderPass(device, &passInfo, nullptr, &renderPass), "Create render pass");

        VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(TriangleData)};
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &push;
        check(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &layout), "Create pipeline layout");

        VkShaderModule vertex = compile(VertexShader, shaderc_vertex_shader);
        VkShaderModule fragment = VK_NULL_HANDLE;
        try
        {
            fragment = compile(FragmentShader, shaderc_fragment_shader);
        }
        catch (...)
        {
            vkDestroyShaderModule(device, vertex, nullptr);
            throw;
        }
        std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
        stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertex;
        stages[0].pName = ShaderEntry;
        stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragment;
        stages[1].pName = ShaderEntry;
        VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState blend{};
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blending{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blending.attachmentCount = 1;
        blending.pAttachments = &blend;
        const VkDynamicState states[]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = static_cast<uint32_t>(std::size(states));
        dynamic.pDynamicStates = states;
        VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        info.stageCount = static_cast<uint32_t>(stages.size());
        info.pStages = stages.data();
        info.pVertexInputState = &input;
        info.pInputAssemblyState = &assembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &samples;
        info.pColorBlendState = &blending;
        info.pDynamicState = &dynamic;
        info.layout = layout;
        info.renderPass = renderPass;
        const VkResult result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
        vkDestroyShaderModule(device, fragment, nullptr);
        vkDestroyShaderModule(device, vertex, nullptr);
        check(result, "Create pipeline");
    }

    void createSwapchain()
    {
        VkSurfaceCapabilitiesKHR caps{};
        check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, &caps), "Get surface capabilities");
        uint32_t count = 0;
        check(vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &count, nullptr), "List formats");
        std::vector<VkSurfaceFormatKHR> formats(count);
        check(vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &count, formats.data()), "List formats");
        if (formats.empty())
        {
            throw std::runtime_error("No surface format");
        }

        // UNORM 保持 Cornflower 的 RGB 值为 100、149、237。
        VkSurfaceFormatKHR chosen = formats.front();
        if (chosen.format == VK_FORMAT_UNDEFINED)
        {
            chosen.format = VK_FORMAT_B8G8R8A8_UNORM;
        }
        for (const auto& available : formats)
        {
            if (available.format == VK_FORMAT_B8G8R8A8_UNORM &&
                available.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            {
                chosen = available;
                break;
            }
        }
        format = chosen.format;
        extent = caps.currentExtent;
        if (extent.width == std::numeric_limits<uint32_t>::max())
        {
            RECT client{};
            GetClientRect(window, &client);
            extent.width = std::clamp(static_cast<uint32_t>(client.right), caps.minImageExtent.width, caps.maxImageExtent.width);
            extent.height = std::clamp(static_cast<uint32_t>(client.bottom), caps.minImageExtent.height, caps.maxImageExtent.height);
        }
        uint32_t imageCount = caps.minImageCount + 1;
        if (caps.maxImageCount && imageCount > caps.maxImageCount)
        {
            imageCount = caps.maxImageCount;
        }
        VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        const VkCompositeAlphaFlagBitsKHR alphaModes[]{VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
            VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR};
        for (auto mode : alphaModes)
        {
            if (caps.supportedCompositeAlpha & mode)
            {
                alpha = mode;
                break;
            }
        }

        VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        info.surface = surface;
        info.minImageCount = imageCount;
        info.imageFormat = format;
        info.imageColorSpace = chosen.colorSpace;
        info.imageExtent = extent;
        info.imageArrayLayers = 1;
        info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.preTransform = caps.currentTransform;
        info.compositeAlpha = alpha;
        info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        info.clipped = VK_TRUE;
        check(vkCreateSwapchainKHR(device, &info, nullptr, &swapchain), "Create swapchain");
        check(vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr), "List images");
        images.resize(count);
        check(vkGetSwapchainImagesKHR(device, swapchain, &count, images.data()), "List images");
        createPipeline();
        views.resize(count);
        frames.resize(count);
        presented.resize(count);
        for (uint32_t index = 0; index < count; ++index)
        {
            VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view.image = images[index];
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = format;
            view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            check(vkCreateImageView(device, &view, nullptr, &views[index]), "Create image view");
            VkFramebufferCreateInfo frame{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            frame.renderPass = renderPass;
            frame.attachmentCount = 1;
            frame.pAttachments = &views[index];
            frame.width = extent.width;
            frame.height = extent.height;
            frame.layers = 1;
            check(vkCreateFramebuffer(device, &frame, nullptr, &frames[index]), "Create framebuffer");
            VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            check(vkCreateSemaphore(device, &semaphore, nullptr, &presented[index]), "Create present semaphore");
        }
        resized = false;
    }

    void createCommands()
    {
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = queueFamily;
        check(vkCreateCommandPool(device, &poolInfo, nullptr, &pool), "Create command pool");
        VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocation.commandPool = pool;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(device, &allocation, &command), "Allocate command buffer");
        VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        check(vkCreateSemaphore(device, &semaphore, nullptr, &acquired), "Create acquire semaphore");
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        check(vkCreateFence(device, &fenceInfo, nullptr, &fence), "Create fence");
    }

    void record(uint32_t index)
    {
        check(vkResetCommandBuffer(command, 0), "Reset command buffer");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        check(vkBeginCommandBuffer(command, &begin), "Begin commands");
        VkClearValue clear{};
        std::copy_n(&Cornflower[0], Cornflower.length(), clear.color.float32);
        // SRGB 附件会编码，先解码背景以保持指定 RGB。
        if (format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_R8G8B8A8_SRGB)
        {
            constexpr float SrgbThreshold = 0.04045f;
            constexpr float SrgbScale = 12.92f;
            constexpr float SrgbOffset = 0.055f;
            constexpr float SrgbGamma = 2.4f;
            for (uint32_t channel = 0; channel < ColorChannels; ++channel)
            {
                const float value = Cornflower[channel];
                clear.color.float32[channel] = value <= SrgbThreshold ? value / SrgbScale :
                    glm::pow((value + SrgbOffset) / (1.0f + SrgbOffset), SrgbGamma);
            }
        }
        VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        pass.renderPass = renderPass;
        pass.framebuffer = frames[index];
        pass.renderArea.extent = extent;
        pass.clearValueCount = 1;
        pass.pClearValues = &clear;
        vkCmdBeginRenderPass(command, &pass, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        const VkViewport viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f};
        const VkRect2D scissor{{0, 0}, extent};
        vkCmdSetViewport(command, 0, 1, &viewport);
        vkCmdSetScissor(command, 0, 1, &scissor);

        // 等比例缩放顶点；push constants 省去顶点缓冲。
        constexpr float TriangleRadius = 0.65f;
        const float aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);
        const glm::vec2 scale = TriangleRadius * glm::vec2(std::min(1.0f, 1.0f / aspect), std::min(1.0f, aspect));
        const glm::vec2 top = glm::vec2(0.0f, -1.0f) * scale;
        const glm::vec2 left = glm::vec2(-glm::sqrt(3.0f) / 2.0f, 0.5f) * scale;
        const glm::vec2 right = glm::vec2(glm::sqrt(3.0f) / 2.0f, 0.5f) * scale;
        const TriangleData vertices{
            glm::vec4(top, 0.0f, 1.0f), glm::vec4(left, 0.0f, 1.0f), glm::vec4(right, 0.0f, 1.0f),
            glm::vec4(1.0f, 0.0f, 0.0f, 1.0f), glm::vec4(0.0f, 1.0f, 0.0f, 1.0f), glm::vec4(0.0f, 0.0f, 1.0f, 1.0f)};
        vkCmdPushConstants(command, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(vertices), vertices.data());
        vkCmdDraw(command, VertexCount, 1, 0, 0);
        vkCmdEndRenderPass(command);
        check(vkEndCommandBuffer(command), "End commands");
    }

    void draw()
    {
        // 单帧在途；呈现信号量按交换链图像分配，避免过早复用。
        check(vkWaitForFences(device, 1, &fence, VK_TRUE, InfiniteTimeout), "Wait for frame");
        uint32_t index = 0;
        const VkResult acquire = vkAcquireNextImageKHR(device, swapchain, InfiniteTimeout, acquired, VK_NULL_HANDLE, &index);
        if (acquire == VK_ERROR_OUT_OF_DATE_KHR)
        {
            resized = true;
            return;
        }
        if (acquire != VK_SUBOPTIMAL_KHR)
        {
            check(acquire, "Acquire image");
        }
        record(index);
        check(vkResetFences(device, 1, &fence), "Reset fence");
        const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &acquired;
        submit.pWaitDstStageMask = &stage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &presented[index];
        check(vkQueueSubmit(queue, 1, &submit, fence), "Submit frame");
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &presented[index];
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain;
        present.pImageIndices = &index;
        const VkResult result = vkQueuePresentKHR(queue, &present);
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || acquire == VK_SUBOPTIMAL_KHR)
        {
            resized = true;
        }
        if (result != VK_ERROR_OUT_OF_DATE_KHR && result != VK_SUBOPTIMAL_KHR)
        {
            check(result, "Present frame");
        }
    }

    void clearSwapchain()
    {
        for (auto frame : frames)
        {
            vkDestroyFramebuffer(device, frame, nullptr);
        }
        frames.clear();
        vkDestroyPipeline(device, pipeline, nullptr);
        vkDestroyPipelineLayout(device, layout, nullptr);
        vkDestroyRenderPass(device, renderPass, nullptr);
        pipeline = VK_NULL_HANDLE;
        layout = VK_NULL_HANDLE;
        renderPass = VK_NULL_HANDLE;
        for (auto view : views)
        {
            vkDestroyImageView(device, view, nullptr);
        }
        views.clear();
        for (auto semaphore : presented)
        {
            vkDestroySemaphore(device, semaphore, nullptr);
        }
        presented.clear();
        vkDestroySwapchainKHR(device, swapchain, nullptr);
        swapchain = VK_NULL_HANDLE;
    }

    ~Triangle()
    {
        // 先等待 GPU，再按依赖顺序释放；初始化失败也可清理。
        if (device)
        {
            vkDeviceWaitIdle(device);
            clearSwapchain();
            vkDestroyFence(device, fence, nullptr);
            vkDestroySemaphore(device, acquired, nullptr);
            vkDestroyCommandPool(device, pool, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        if (surface)
        {
            vkDestroySurfaceKHR(instance, surface, nullptr);
        }
        if (messenger)
        {
            auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
            destroy(instance, messenger, nullptr);
        }
        if (instance)
        {
            vkDestroyInstance(instance, nullptr);
        }
        if (window)
        {
            DestroyWindow(window);
        }
        UnregisterClassW(WindowName, module);
    }

    void run()
    {
        createWindow();
        createInstance();
        createDevice();
        createCommands();
        createSwapchain();
        ShowWindow(window, SW_SHOW);
        MSG message{};
        while (true)
        {
            if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            {
                if (message.message == WM_QUIT)
                {
                    break;
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
                continue;
            }

            // 最小化暂停，窗口尺寸改变时重建交换链。
            RECT client{};
            GetClientRect(window, &client);
            if (IsIconic(window) || client.right == 0 || client.bottom == 0)
            {
                WaitMessage();
                continue;
            }
            if (resized)
            {
                check(vkDeviceWaitIdle(device), "Wait before resize");
                clearSwapchain();
                createSwapchain();
            }
            draw();
        }
    }
};

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    try
    {
        Triangle app;
        app.run();
        return 0;
    }
    catch (const std::exception& error)
    {
        OutputDebugStringA(error.what());
        std::fprintf(stderr, "%s\n", error.what());
        MessageBoxA(nullptr, error.what(), "Vulkan error", MB_OK | MB_ICONERROR);
        return 1;
    }
}
