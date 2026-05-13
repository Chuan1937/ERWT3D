/**
 * @file main.cpp
 * @brief 三维浮点数据生成器程序入口
 * 
 * 该程序用于生成三维浮点数据集并写入本地文件。
 * 数据按X-Y-Z三个维度的顺序存储。
 * 
 * @author 3DDataGenerator
 * @version 1.0.0
 * @date 2024
 */

#include <QApplication>
#include <QFont>
#include "MainWindow.h"

int main(int argc, char *argv[])
{
    // 创建应用程序实例
    QApplication app(argc, argv);
    
    // 设置应用程序信息
    app.setApplicationName("3DDataGenerator");
    app.setApplicationVersion("1.0.0");
    app.setOrganizationName("3DDataGenerator");
    
    // 设置默认字体
    QFont font = app.font();
    font.setFamily("Microsoft YaHei");  // 微软雅黑
    font.setPointSize(9);
    app.setFont(font);
    
    // 创建并显示主窗口
    MainWindow mainWindow;
    mainWindow.show();
    
    // 运行应用程序事件循环
    return app.exec();
}
