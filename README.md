# ESP32-CV

On-device object detection with a Wi-Fi web interface.

| Specification | Details |
| --- | --- |
| Board | ESP32-S3-WROOM-1 N16R8 CAM |
| CPU frequency | 240 MHz |
| Memory | 16 MB flash, 8 MB octal PSRAM at 40 MHz |
| Camera | OV5640, 320 × 320 pixels, RGB565 |
| Detection model | YOLO11n, COCO, 80 object classes |
| Model input | 320 × 320 × 3, INT8 quantization |
| Inference | Local execution using Espressif ESP-DL / COCO Detect |
| Firmware | ESP-IDF, built with PlatformIO |
| Connectivity | 2.4 GHz Wi-Fi, station mode |
| Web interface | Camera capture, JPEG upload, adjustable confidence threshold, detection results and LED control |
| Monitoring | CPU usage, memory, internal temperature and processing times |

[Screenshot](images/screenshot.png)
