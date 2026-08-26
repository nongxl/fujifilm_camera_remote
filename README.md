# 富士相机全功能智能遥控器 (Fujifilm Camera Smart Remote)

本项目旨在为 M5Stack StickS3 (ESP32-S3) 开发一款全功能的富士相机遥控器。
突破传统蓝牙遥控器的限制，通过 Wi-Fi 及 PTP/IP 协议实现高级相机控制功能。

## 🎯 核心目标与特性

*   **硬件平台**: M5Stack StickS3 (基于 ESP32-S3，带屏幕和按键)。
*   **核心功能**:
    *   **实时取景 (Live View)**: 接收相机 MJPEG 视频流并在屏幕上实时渲染。
    *   **参数调节 (Parameter Control)**: 实时读取和修改 ISO、光圈、快门速度、曝光补偿等。
    *   **快门控制 (Shutter Release)**: 拍照、半按对焦、长曝光锁定。
    *   **多品牌架构 (Multi-brand Ready)**: 预留索尼、佳能等其他相机的控制接口架构。

## 🗺️ 开发计划 (Step-by-Step Plan)

结合开源项目 `furble`（优秀的硬件UI底座）和 `studio-camera`（优秀的协议实现），我们分为以下几个阶段推进：

### 阶段一：基础环境与 UI 框架搭建 (Base & UI)
1.  **PlatformIO 环境配置**: 针对 M5StickS3 优化编译选项，开启 PSRAM 支持（这对后续图像解码至关重要）。
2.  **集成硬件驱动库**: 引入 `M5Unified` 库控制屏幕和按键。
3.  **移植 LVGL 框架**: 参考 `furble`，搭建基于 LVGL 的基础用户界面结构（状态栏、主菜单、参数显示区、取景器占位符）。

### 阶段二：网络层与相机连接 (Wi-Fi & Connection)
1.  **Wi-Fi 扫描与连接**: 实现扫描周边 Wi-Fi 热点，并连接到富士相机的热点。
2.  **网络通信基础**: 封装基于 ESP32 的 TCP 客户端 (用于 PTP 控制指令) 和 UDP 客户端 (用于事件监听)。
3.  **PTP/IP 握手协议**: 参考 `studio-camera`，用 C++ 实现 PTP/IP 的初始化连接 (InitCommandRequest, InitEventRequest)。

### 阶段三：相机控制与参数同步 (Control & Parameters)
1.  **实现 PTP 控制指令**: 封装通用的 PTP 操作码 (Operation Code) 发送和响应解析机制。
2.  **获取相机状态**: 发送指令获取当前的 ISO、光圈、快门等参数，并同步更新到 LVGL 界面上。
3.  **双向控制**: 实现通过物理按键或触屏（如有）修改参数，并发送控制指令到相机；实现快门触发指令。

### 阶段四：实时取景 (Live View)
1.  **获取 MJPEG 流**: 触发相机的实时取景开启指令，并解析返回的视频流数据帧。
2.  **图像解码与渲染**: 在 ESP32-S3 上集成高效的 JPEG 解码库 (如 `TJpg_Decoder` 或利用 ESP32-S3 的部分硬件加速特性)。
3.  **性能优化**: 优化解码缓冲区和双缓存刷新策略，确保在小屏幕上获得可接受的帧率，不阻塞 UI 线程。

### 阶段五：架构优化与扩展 (Optimization & Extension)
1.  **抽象出基类接口 (Camera Interface)**: 将“连接、取流、参数获取/设置、快门”抽象为纯虚类。
2.  **剥离富士特定代码**: 使得 PTP/IP 模块成为实现该接口的一个派生类。
3.  为未来的索尼 (REST API/JSON-RPC) 等相机预留框架位置。

## 🛠️ 技术栈
*   C++ / PlatformIO
*   M5Unified (硬件驱动)
*   LVGL (图形界面)
*   TCP/IP Sockets (WiFi 通信)
*   PTP/IP Protocol (富士相机通信协议)
