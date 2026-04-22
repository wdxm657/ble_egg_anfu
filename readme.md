### Application File
- app.c：用户主文件，用于完成BLE协议栈初始化、数据处理、低功耗处理等。
- app_att.c：service和profile的配置文件，有Telink定义的Attribute结构，根据该结构，已提供GATT、标准HID和私有的OTA、MIC等相关Attribute，用户可以参考这些添加自己的service和profile。
- app_buffer.c：用于配置ACL RX FIFO、ACL TX FIFO、L2CAP RX Buffer等。
- app_ui.c：按键、OTA等用户任务的处理文件。