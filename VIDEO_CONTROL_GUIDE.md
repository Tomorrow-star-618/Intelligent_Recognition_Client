# 视频流控制逻辑说明

## 📋 控制命令对比表

| 操作ID | 命令格式 | 功能 | 影响范围 | 用途 |
|--------|---------|------|---------|------|
| **9** | `DEVICE_2:OP_9:VALUE_1/0` | 开启/关闭RTSP推流 | 仅编码推流 | 临时停止推流，节省网络带宽 |
| **10** | `DEVICE_2:OP_10:VALUE_1/0` | 恢复/暂停所有线程 | 所有4个线程 | 完全暂停视频处理，节省CPU资源 |

---

## 🔍 两种控制方式的区别

### 方式1：操作ID 9（仅控制RTSP推流）

**命令示例：**
```
# 关闭RTSP推流
DEVICE_2:OP_9:VALUE_0

# 开启RTSP推流
DEVICE_2:OP_9:VALUE_1
```

**效果：**
```
关闭推流 (VALUE_0):
✅ 采集线程：继续运行（30fps）
✅ BGR转换线程：继续运行（27fps）
✅ 推理线程：继续运行（每5帧推理1次）
❌ 编码推流线程：停止RTSP推流

开启推流 (VALUE_1):
✅ 所有线程：继续运行
✅ RTSP推流：恢复推流
```

**资源消耗：**
- CPU占用：约70%（仍在采集、转换、推理）
- 网络带宽：0 Mbps（无推流）
- 内存占用：正常（缓冲区仍在使用）

**适用场景：**
- ✅ 客户端临时不需要看视频，但需要继续AI检测
- ✅ 网络带宽受限，暂时停止推流
- ✅ 录像功能（继续采集但不推流）

---

### 方式2：操作ID 10（暂停所有线程）⭐ 推荐

**命令示例：**
```
# 暂停所有线程
DEVICE_2:OP_10:VALUE_0

# 恢复所有线程
DEVICE_2:OP_10:VALUE_1
```

**效果：**
```
暂停所有线程 (VALUE_0):
❌ 采集线程：暂停（VI硬件保持激活）
❌ BGR转换线程：暂停
❌ 推理线程：暂停
❌ 编码推流线程：暂停

恢复所有线程 (VALUE_1):
✅ 采集线程：重启
✅ BGR转换线程：重启
✅ 推理线程：重启
✅ 编码推流线程：重启
```

**资源消耗：**
- CPU占用：约5%（仅主循环运行）
- 网络带宽：0 Mbps（无推流）
- 内存占用：正常（缓冲区保留）

**适用场景：**
- ✅ 设备进入待机模式
- ✅ 长时间不需要视频和AI检测
- ✅ 节省电量和CPU资源
- ✅ 夜间自动暂停功能

---

## 🎯 实际使用建议

### 场景1：客户端断开连接
```
问题：客户端关闭APP，设备还在空转推流

解决方案：
1. 客户端断开前发送：DEVICE_2:OP_10:VALUE_0（暂停所有线程）
2. 客户端重连后发送：DEVICE_2:OP_10:VALUE_1（恢复所有线程）

效果：节省约65% CPU资源
```

### 场景2：夜间自动待机
```
问题：晚上不需要监控，设备24小时运行浪费资源

解决方案：
1. 晚上23:00发送：DEVICE_2:OP_10:VALUE_0（暂停）
2. 早上07:00发送：DEVICE_2:OP_10:VALUE_1（恢复）

效果：8小时节省约5W电力
```

### 场景3：网络不稳定
```
问题：网络断开，RTSP推流失败但线程仍在运行

解决方案：
方案A（仅停推流）：DEVICE_2:OP_9:VALUE_0
方案B（完全暂停）：DEVICE_2:OP_10:VALUE_0

推荐方案A（保持AI检测，网络恢复后立即推流）
```

---

## 🔧 技术实现细节

### 操作ID 9 的实现
```cpp
void Video::stopRTSP() {
    rtsp_enable_ = false;  // 仅设置标志位
    // running_ 仍为 true，4个线程继续运行
}

// 编码线程中的判断
if (rtsp_enable_ && g_rtsp_session_) {
    rtsp_tx_video(g_rtsp_session_, ...);  // 只有rtsp_enable_=true才推流
}
```

### 操作ID 10 的实现
```cpp
void Video::pauseAllThreads() {
    running_ = false;  // ⚠️ 关键：所有线程检测到false会退出
    
    // 等待4个线程退出
    pthread_join(thread_capture_, NULL);
    pthread_join(thread_bgr_convert_, NULL);
    pthread_join(thread_inference_, NULL);
    pthread_join(thread_encode_, NULL);
}

void Video::resumeAllThreads() {
    running_ = true;  // 重新设置运行标志
    
    // 重新创建4个线程
    pthread_create(&thread_capture_, ...);
    pthread_create(&thread_bgr_convert_, ...);
    pthread_create(&thread_inference_, ...);
    pthread_create(&thread_encode_, ...);
}
```

---

## ⚠️ 注意事项

### 1. 硬件资源不释放
暂停线程时，**VI采集、VENC编码器等硬件资源仍保持激活状态**，以便快速恢复。

如需完全释放资源，请调用 `stop()` 方法（会销毁所有资源）。

### 2. 缓冲区数据保留
暂停时，三缓冲区中的数据不会清空，恢复后会继续处理旧数据。

如需清空缓冲区，请在恢复后等待3帧（约100ms）。

### 3. ONVIF服务不受影响
暂停/恢复线程不会影响ONVIF服务（端口8080仍在监听）。

客户端仍可通过ONVIF获取设备信息，只是RTSP流会中断。

---

## 📊 性能对比

| 状态 | CPU占用 | 网络带宽 | 功能 |
|------|---------|---------|------|
| **正常运行** | 70% | 2 Mbps | 采集+推理+推流 |
| **停止RTSP推流（ID 9）** | 70% | 0 Mbps | 采集+推理 |
| **暂停所有线程（ID 10）** | 5% | 0 Mbps | 仅主循环 |
| **完全停止（stop）** | 2% | 0 Mbps | 资源已释放 |

---

## 🎮 Qt客户端集成示例

```cpp
// Qt客户端代码
void MainWindow::onPauseVideo() {
    // 方案1：仅停止推流（推荐，快速恢复）
    tcpClient->sendCommand("DEVICE_2:OP_9:VALUE_0");
    
    // 方案2：暂停所有线程（省资源，恢复慢）
    tcpClient->sendCommand("DEVICE_2:OP_10:VALUE_0");
}

void MainWindow::onResumeVideo() {
    // 方案1：恢复推流（约50ms恢复）
    tcpClient->sendCommand("DEVICE_2:OP_9:VALUE_1");
    
    // 方案2：恢复所有线程（约200ms恢复）
    tcpClient->sendCommand("DEVICE_2:OP_10:VALUE_1");
}
```

---

## 📝 总结

1. **操作ID 9**：轻量级控制，仅停止RTSP推流，适合短暂暂停
2. **操作ID 10**：重量级控制，暂停所有线程，适合长时间待机

根据您的需求选择合适的控制方式！
