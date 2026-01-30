# 项目源码结构说明

本项目是一个基于 RV1106 (Luckfox Pico) 的智能识别客户端，实现了视频采集、AI 推理 (YOLOv5)、RTSP 推流、TCP/UDP 通信以及舵机控制等功能。

项目源码位于 `code` 目录下，主要分为 `src` (主程序)、`modules` (功能模块)、`include` (公共头文件) 三大类。

## 1. 核心目录 (`code/src`)

| 文件 | 功能描述 |
| :--- | :--- |
| `main.cc` | **主程序入口**。负责初始化所有模块（Video, Servo, TcpClient, UdpDiscovery, Control），处理信号（SIGTERM），并管理主循环。包含获取本机IP和RTSP URL生成的逻辑。 |

## 2. 功能模块 (`code/modules`)

每个子目录对应一个独立的功能模块。

### 2.1 视频与推理 (`code/modules/video`)
核心模块，采用四线程架构（采集 -> BGR转换 -> 推理 -> 编码推流）。

| 文件 | 功能描述 |
| :--- | :--- |
| `video.h` / `.cc` | **视频处理核心类**。封装了四线程架构，管理视频流的生命周期，协调采集、推理和推流。支持AI识别、区域检测、ONVIF集成等功能。 |
| `luckfox_mpi.h` / `.cc` | **硬件媒体接口抽象**。封装了瑞芯微 MPI (Media Process Interface) 接口，负责 VI (视频输入)、VENC (视频编码)、RGA (2D图形加速) 等硬件资源的操作。 |

### 2.2 AI 模型 (`code/modules/yolo`)
负责 YOLOv5 模型的加载、推理和结果处理。

| 文件 | 功能描述 |
| :--- | :--- |
| `yolov5.cc` | **RKNN 模型接口**。负责加载 `.rknn` 模型，管理 RKNN 上下文，输入输出内存分配，以及执行推理 (`rknn_run`)。 |
| `postprocess.cc` | **后处理逻辑**。负责解析 RKNN 推理输出的原始张量数据，进行阈值过滤、NMS (非极大值抑制) 处理，生成最终的检测框和类别。 |

### 2.3 网络通信 (`code/modules/tcp` & `code/modules/udp_discovery` & `code/modules/onvif`)
负责与上位机或 NVR 交互。

| 文件 | 功能描述 |
| :--- | :--- |
| `tcp/tcp.h` / `.cc` | **TCP 客户端**。负责与上位机建立 TCP 连接，发送状态数据，接收控制命令。支持自动重连和动态更新服务器地址。 |
| `udp_discovery/udp_discovery.h` / `.cc` | **UDP 设备发现**。监听广播端口 (8888)，响应 `discovery_request`，发送心跳包，处理 `connection_request` 并通过回调触发 TCP 连接。 |
| `onvif/onvif_server.h` / `.cc` | **ONVIF 服务器**。实现 ONVIF Profile S 协议，支持标准 NVR 或客户端（如 ODM）发现设备并获取 RTSP 流地址。 |

### 2.4 控制中心 (`code/modules/control`)
负责业务逻辑的调度。

| 文件 | 功能描述 |
| :--- | :--- |
| `control.h` / `.cc` | **命令控制器**。接收来自 TCP 或命令行的指令（如云台控制、AI开关、ROI设置），解析后调用对应的模块（Servo, Video）执行操作。 |

### 2.5 硬件控制 (`code/modules/servo`)
负责外设控制。

| 文件 | 功能描述 |
| :--- | :--- |
| `servo.h` / `.cc` | **舵机控制接口**。提供高层接口控制水平和垂直舵机的转动角度。 |
| `pwm.h` / `.cc` | **PWM 底层驱动**。直接操作 Linux PWM 子系统 (sysfs)，产生控制舵机所需的 PWM 信号。 |

## 3. 公共资源 (`code/include` & `code/model`)

| 文件 | 功能描述 |
| :--- | :--- |
| `include/common.h` | **公共定义**。定义了通用数据结构（如 `RectInfo`）和 COCO 数据集的 80 个类别名称。 |
| `model/` | 存放 `.rknn` 模型文件及锚框配置 (`anchors_yolov5.txt`)。 |

---
**总结**：项目采用模块化设计，`main.cc` 作为胶水代码将各模块组装。`Video` 模块是数据流处理的核心，`Control` 模块是指令流处理的核心，网络模块负责对外交互。
