用 VS2026 打开 `Vulkan.sln`，选择 `Debug | x64`，按 F5。

依赖 Vulkan SDK 1.4.363.0，使用其 GLM 和 shaderc。SDK 路径由 `Vulkan.vcxproj` 的 `VulkanSdkDir` 指定；默认兼容本机安装路径。

窗口和事件使用 GLFW 3.5.1，路径为 `../glfw-3.5.1.bin.WIN64`，链接 `lib-vc2026/glfw3dll.lib`。

全部程序与 GLSL 位于 `main.cpp`。背景为 Cornflower（RGB 100、149、237）；左侧三角形顶点为上红、左绿、右蓝，右侧正方形采样 C++ 生成的 8×8 灰白棋盘格。支持缩放、最小化；Esc 退出。

输出：`bin/x64/Debug/Vulkan.exe` 或 `bin/x64/Release/Vulkan.exe`。构建自动复制 `shaderc_shared.dll`、`glfw3.dll`，运行时需与 EXE 同目录。Debug 可用时启用 Vulkan 验证层。错误写入标准错误流。
