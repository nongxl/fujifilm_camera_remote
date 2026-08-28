# ESP32-S3 极速 30+ FPS 相机实时取景 (LiveView) 串流全链路优化技术指南

> **适用场景**：适用于基于 ESP32-S3（如 M5StickC S3、M5Cardputer、ESP32-S3 开发板）接收并渲染数码相机（富士、理光 GR、索尼、佳能等）或网络摄像头的 **MJPEG / JPEG Wi-Fi 视频流**，将帧率从最初卡顿的 **4~5 FPS** 极限推升至 **30+ FPS（达到硬件物理上限）**。

---

## 目录
1. [性能瓶颈演进与突破全景图](#一性能瓶颈演进与突破全景图)
2. [第一重优化：Wi-Fi 射频与网络底层拉满（突破 5fps 上限）](#二第一重优化wi-fi-射频与网络底层拉满突破-5fps-上限)
3. [第二重优化：消除隐式休眠与流式 SOI/EOI 定界（突破 10fps 上限）](#三第二重优化消除隐式休眠与流式-soieoi-定界突破-10fps-上限)
4. [第三重优化：ESP32-S3 双核并行异步流水线（突破 20fps 上限）](#四第三重优化esp32-s3-双核并行异步流水线突破-20fps-上限)
5. [第四重优化：乐鑫官方 SIMD 硬件矢量解码 `esp_new_jpeg`（单帧 2.8ms）](#五第四重优化乐鑫官方-simd-硬件矢量解码-esp_new_jpeg单帧-28ms)
6. [第五重优化：零拷贝 Ping-Pong 内存直推与 SPI DMA 渲染](#六第五重优化零拷贝-ping-pong-内存直推与-spi-dma-渲染)
7. [Cardputer Camera 项目移植实战指南](#七cardputer-camera-项目移植实战指南)

---

## 一、性能瓶颈演进与突破全景图

| 优化阶段 | 核心痛点与限制因素 | 采取的突破方案 | 实测帧率表现 |
| :--- | :--- | :--- | :--- |
| **初始状态** | 默认 Wi-Fi Modem DTIM 休眠；CPU 软件双线性插值解码（单帧耗时 60ms） | 基础纯软解流程 | **3 ~ 4 FPS** |
| **阶段 1** | Wi-Fi 射频每秒仅唤醒拉取 ~5 次数据包；软解严重阻塞 CPU | 禁用 `WIFI_PS_MIN_MODEM`；扩大 TCP 接收窗 | **5 ~ 8 FPS** |
| **阶段 2** | 分包读取循环中包含 `delay(1)`（FreeRTOS 1 Tick = 10ms），每帧白白沉睡 100ms | 移除所有人工延时；引入非阻塞 POSIX `recv` | **9 ~ 12 FPS** |
| **阶段 3** | 单核单线程下“收包”与“推屏”互相串行阻塞，TCP 接收窗口频繁断流 | 架构升级为 **Core 0（收包）+ Core 1（SIMD 解码/推屏）双核并行流水线** | **20 ~ 25 FPS** |
| **阶段 4** | 帧定界依赖长度计算易失步；内存反复拷贝与局部大数组栈溢出 | **流式 SOI/EOI 状态机** + **Ping-Pong 零拷贝指针原子互换** + **SIMD 频域降采样** | **30+ FPS（丝滑满帧）** |

---

## 二、第一重优化：Wi-Fi 射频与网络底层拉满（突破 5fps 上限）

### 1. 强制关闭 Wi-Fi Modem 节能休眠（极度致命）
* **原因**：ESP32 默认开启了 `WIFI_PS_MIN_MODEM`，芯片在无发送时会根据 AP 的 DTIM 周期（通常 100~200ms）自动切断射频休眠。导致每秒最多只能接收 5~6 次 TCP 包。
* **代码实现**：
```cpp
#include <esp_wifi.h>
#include <WiFi.h>

// 在 Wi-Fi 连接成功后必须立刻执行：
WiFi.setSleep(false);
esp_wifi_set_ps(WIFI_PS_NONE); // 彻底关闭 Wi-Fi 射频休眠
```

### 2. 启用 `TCP_NODELAY` 与 32KB Socket 接收缓冲
* **原因**：默认 Socket 接收缓冲区只有 4KB~8KB，相机爆发式发送 18KB 帧时极易发生 TCP Window Full，导致相机端暂停推流。
* **代码实现**：
```cpp
#include <lwip/sockets.h>

int fd = client.fd();
if (fd >= 0) {
    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    int rcvbuf = 32768; // 32KB 接收缓冲
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
}
```

### 3. Wi-Fi 与 BLE 射频时分复用释放（针对带蓝牙机型）
* **原理**：ESP32-S3 的 Wi-Fi 和 BLE 共用同一套 2.4GHz 射频天线。取景期间保持 BLE 连接会导致 Wi-Fi 吞吐量暴跌 **54%**（实测从 831 KiB/s 跌至 380 KiB/s）。
* **策略**：取景流建立稳定 3 秒后，主动断开/挂起 BLE 链路；需要触发快门或退出取景时再快速重连。

---

## 三、第二重优化：消除隐式休眠与流式 SOI/EOI 定界（突破 10fps 上限）

### 1. 彻底根除分包循环中的 `delay(1)`
* **原因**：一帧 18KB 的 JPEG 会被 Wi-Fi MTU 拆分为约 12 个分包。如果代码在分包间执行 `delay(1)`，在 FreeRTOS 下每次 `delay(1)` 等于挂起 **10 毫秒**，12 个包累计沉睡 **100~120ms**，直接将 FPS 锁死在 10fps 以下！
* **对策**：采用纯非阻塞 POSIX `recv(fd, buf, len, MSG_DONTWAIT)`。当前底层有数据就瞬间读完，没数据立刻让出，绝不执行任何 `delay()`。

### 2. 采用标准 MJPEG 流式 SOI (`0xFF 0xD8`) / EOI (`0xFF 0xD9`) 帧定界
* **原理**：摒弃不可靠的“4 字节头部长度计算+滑动查找”，直接按字节流状态机检测 JPEG 标志：
  - 检测到 `0xFF 0xD8`：立即开启新帧组装；
  - 遇到 `0xFF 0xD9`：帧数据 100% 完整接收，立即提交渲染。

---

## 四、第三重优化：ESP32-S3 双核并行异步流水线（突破 20fps 上限）

### 1. 单核串行 vs 双核并行对比

```
【旧架构：单核串行阻塞 (耗时叠加)】
Core 1: [收包 12ms] -> [SIMD解码 3ms] -> [DMA推屏 4ms] -> [收包 12ms] ...
周期 = 19ms (最大理论帧率仅 ~50fps，若遇网络抖动瞬间跌至 10~15fps)

【新架构：双核异步流水线 (耗时完全掩盖)】
Core 0 (网络专核): ===[收第 1 帧 12ms]===> ===[收第 2 帧 12ms]===> ===[收第 3 帧 12ms]===>
Core 1 (渲染专核):                        ===[解/推第 1 帧 7ms]==> ===[解/推第 2 帧 7ms]==>
渲染与网络接收 100% 并行，相机推流吞吐达到硬件极限！
```

### 2. 核心代码实现

```cpp
// 1. 创建 Core 0 专用网络接收任务
xTaskCreatePinnedToCore(
    rxTaskTrampoline,
    "LiveViewRx",
    8192,               // 8KB 任务栈空间
    this,
    5,                  // 高优先级
    &m_rxTaskHandle,
    0                   // 绑定 Core 0（与 lwIP TCP 协议栈同核）
);

// 2. Core 0 任务主循环 (高效批量吞吐)
void FujiLiveViewStream::rxTaskLoop() {
    while (m_running) {
        int bytesRead = recv(fd, m_rxBuffer, RX_BUFFER_SIZE, MSG_DONTWAIT);
        if (bytesRead > 0) {
            processStreamData(m_rxBuffer, bytesRead);
        } else {
            vTaskDelay(1); // 仅在 Socket 读空时微让步
        }
    }
}
```

---

## 五、第四重优化：乐鑫官方 SIMD 硬件矢量解码 `esp_new_jpeg`（单帧 2.8ms）

### 1. 软件解码与硬件 SIMD 解码性能对比
- **通用软解 (`JPEGDEC` / `LovyanGFX drawJpg`)**：CPU 逐像素运算，单帧耗时 **45 ~ 70 ms**。
- **乐鑫 SIMD 矢量硬件解码 (`esp_new_jpeg`)**：利用 ESP32-S3 专属向量汇编指令，单帧耗时仅 **2.8 ms**（提速超过 20 倍）。

### 2. 频域 1/2 硬件下采样机制（无额外 CPU 运算）
相机输出通常为 640×480，而单片机屏幕通常为 240×135 或 320×240。
在解码配置中设置 1/2 分频尺寸（`320×240`）：
```cpp
jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
config.output_type = JPEG_PIXEL_FORMAT_RGB565_BE; // 直接输出大端 RGB565 像素
config.scale.width = 320;                         // 640 / 2 = 320 (频域 1/2 IDCT)
config.scale.height = 240;                        // 480 / 2 = 240 (频域 1/2 IDCT)
jpeg_dec_open(&config, &m_jpeg);
```
> **原理**：解码器在 IDCT 阶段直接丢弃高频系数，仅计算 4×4 低频矩阵，运算量直接暴降 75%，并输出对齐的 RGB565 原始像素，**完全无需 CPU 二次缩放**！

---

## 六、第五重优化：零拷贝 Ping-Pong 内存直推与 SPI DMA 渲染

### 1. Ping-Pong 指针原子互换（0 内存拷贝）
传统的 `memcpy` 拷贝 20KB 帧需要几十微秒。改用双缓冲指针互换仅需 **1 纳秒**：
```cpp
// Core 0 组装完毕后，互换缓冲区指针
portENTER_CRITICAL(&m_frameMux);
uint8_t* temp = m_readyBufPtr;
m_readyBufPtr = m_assembleBufPtr;
m_assembleBufPtr = temp;
m_readyLen = m_assembleLen;
m_hasNewFrame = true;
portEXIT_CRITICAL(&m_frameMux);
```

### 2. Center-Crop 内存行直推与 DMA 推屏
```cpp
// 裁切 320x240 中央区域至 240x135 屏幕（垂直方向 100% 满高无黑边）
const int visibleW = 240;
const int visibleH = 135;
const int cropX = (320 - visibleW) / 2; // 40
const int cropY = (240 - visibleH) / 2; // 52

// 利用 memcpy 行复制直接搬运至 DMA Sprite 画布
for (int row = 0; row < visibleH; ++row) {
    const uint16_t* sRow = pixels + (cropY + row) * 320 + cropX;
    uint16_t* dRow = (uint16_t*)m_canvas.getBuffer() + row * 240;
    memcpy(dRow, sRow, visibleW * sizeof(uint16_t));
}

// DMA 一次性推送到 ST7789 屏幕 (零撕裂、零频闪)
m_canvas.pushSprite(&display, 0, 0);
```

---

## 七、Cardputer Camera 项目移植实战指南

若需将此套极致优化方案移植到 **`D:\workspace\cardputer_Camera`** 项目，请按以下步骤配置：

### 1. 修改 `platformio.ini` 引入 `esp_new_jpeg` 库
```ini
lib_deps =
    https://github.com/espressif/esp-extra-components.git#06e12e1
lib_ignore =
    esp_new_jpeg
extra_scripts =
    pre:scripts/link_esp_new_jpeg.py
```

### 2. 添加链接脚本 `scripts/link_esp_new_jpeg.py`
```python
Import("env")
import os

project_dir = env.get("PROJECT_DIR")
lib_path = os.path.join(
    project_dir,
    ".pio", "libdeps", env.get("PIOENV"), "esp_new_jpeg", "lib", "esp32s3", "libesp_new_jpeg.a"
)

if os.path.exists(lib_path):
    env.Append(LIBPATH=[os.path.dirname(lib_path)])
    env.Append(LIBS=["esp_new_jpeg"])
```

### 3. 移植检查清单 (Checklist)
- [ ] 调用 `WiFi.setSleep(false)` 与 `esp_wifi_set_ps(WIFI_PS_NONE)`。
- [ ] 为 Socket 配置 `TCP_NODELAY` 与 32KB `SO_RCVBUF`。
- [ ] 创建运行在 **Core 0** 上的独立 FreeRTOS 任务负责收包拼帧。
- [ ] 采用 `0xFF 0xD8` (SOI) 与 `0xFF 0xD9` (EOI) 流式状态机。
- [ ] 使用两组 64KB 预分配缓冲进行 Ping-Pong 指针原子交换。
- [ ] 使用 `esp_new_jpeg` 进行 1/2 分频 SIMD 硬件解码（`320×240`）。
- [ ] 使用 `memcpy` 行直推 Center-Crop 写入 DMA Sprite 刷新屏幕。
