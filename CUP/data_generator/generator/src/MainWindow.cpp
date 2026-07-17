#include "MainWindow.h"
#include "DataGenerator.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFileDialog>
#include <QMessageBox>
#include <QApplication>
#include <QStyle>
#include <QScreen>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_workerThread(nullptr)
{
    m_generator = std::make_unique<DataGenerator>();
    setupUI();
    applyStyleSheet();
    updateEstimateInfo();

    // 设置窗口属性
    setWindowTitle(tr("三维浮点数据生成器 v1.0"));
    setMinimumSize(500, 600);
    resize(550, 650);

    // 居中显示窗口
    QScreen *screen = QApplication::primaryScreen();
    QRect screenGeometry = screen->availableGeometry();
    int x = (screenGeometry.width() - width()) / 2;
    int y = (screenGeometry.height() - height()) / 2;
    move(x, y);
}

MainWindow::~MainWindow()
{
    if (m_workerThread && m_workerThread->isRunning()) {
        m_generator->stopGeneration();
        m_workerThread->quit();
        m_workerThread->wait(3000);
    }
}

void MainWindow::setupUI()
{
    QWidget *centralWidget = new QWidget(this);
    QVBoxLayout *mainLayout = new QVBoxLayout(centralWidget);
    mainLayout->setSpacing(15);
    mainLayout->setContentsMargins(20, 20, 20, 20);

    // 添加标题
    QLabel *titleLabel = new QLabel(tr("三维浮点数据生成器"), this);
    titleLabel->setObjectName("titleLabel");
    titleLabel->setAlignment(Qt::AlignCenter);
    mainLayout->addWidget(titleLabel);

    // 添加维度设置组
    mainLayout->addWidget(createDimensionGroup());

    // 添加数值范围设置组
    mainLayout->addWidget(createValueRangeGroup());

    // 添加输出设置组
    mainLayout->addWidget(createOutputGroup());

    // 添加进度显示组
    mainLayout->addWidget(createProgressGroup());

    // 添加弹性空间
    mainLayout->addStretch();

    // 添加按钮面板
    mainLayout->addWidget(createButtonPanel());

    setCentralWidget(centralWidget);
}

QGroupBox* MainWindow::createDimensionGroup()
{
    QGroupBox *group = new QGroupBox(tr("数据维度设置"), this);
    QGridLayout *layout = new QGridLayout(group);
    layout->setSpacing(10);

    // X维度
    QLabel *lblX = new QLabel(tr("X维度大小:"), this);
    m_spinDimX = new QSpinBox(this);
    m_spinDimX->setRange(1, 100000);
    m_spinDimX->setValue(100);
    m_spinDimX->setToolTip(tr("设置X维度的大小（1-100000）"));
    layout->addWidget(lblX, 0, 0);
    layout->addWidget(m_spinDimX, 0, 1);

    // Y维度
    QLabel *lblY = new QLabel(tr("Y维度大小:"), this);
    m_spinDimY = new QSpinBox(this);
    m_spinDimY->setRange(1, 100000);
    m_spinDimY->setValue(100);
    m_spinDimY->setToolTip(tr("设置Y维度的大小（1-100000）"));
    layout->addWidget(lblY, 1, 0);
    layout->addWidget(m_spinDimY, 1, 1);

    // Z维度
    QLabel *lblZ = new QLabel(tr("Z维度大小:"), this);
    m_spinDimZ = new QSpinBox(this);
    m_spinDimZ->setRange(1, 100000);
    m_spinDimZ->setValue(100);
    m_spinDimZ->setToolTip(tr("设置Z维度的大小（1-100000）"));
    layout->addWidget(lblZ, 2, 0);
    layout->addWidget(m_spinDimZ, 2, 1);

    // 连接信号更新预估信息
    connect(m_spinDimX, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &MainWindow::updateEstimateInfo);
    connect(m_spinDimY, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &MainWindow::updateEstimateInfo);
    connect(m_spinDimZ, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &MainWindow::updateEstimateInfo);

    return group;
}

QGroupBox* MainWindow::createValueRangeGroup()
{
    QGroupBox *group = new QGroupBox(tr("数值范围设置"), this);
    QHBoxLayout *layout = new QHBoxLayout(group);
    layout->setSpacing(15);

    // 最小值
    QLabel *lblMin = new QLabel(tr("最小值:"), this);
    m_spinMinValue = new QDoubleSpinBox(this);
    m_spinMinValue->setRange(-1e9, 1e9);
    m_spinMinValue->setValue(0.0);
    m_spinMinValue->setDecimals(6);
    m_spinMinValue->setToolTip(tr("设置随机数的最小值"));
    layout->addWidget(lblMin);
    layout->addWidget(m_spinMinValue);

    layout->addSpacing(20);

    // 最大值
    QLabel *lblMax = new QLabel(tr("最大值:"), this);
    m_spinMaxValue = new QDoubleSpinBox(this);
    m_spinMaxValue->setRange(-1e9, 1e9);
    m_spinMaxValue->setValue(1.0);
    m_spinMaxValue->setDecimals(6);
    m_spinMaxValue->setToolTip(tr("设置随机数的最大值"));
    layout->addWidget(lblMax);
    layout->addWidget(m_spinMaxValue);

    layout->addStretch();

    return group;
}

QGroupBox* MainWindow::createOutputGroup()
{
    QGroupBox *group = new QGroupBox(tr("输出设置"), this);
    QHBoxLayout *layout = new QHBoxLayout(group);
    layout->setSpacing(10);

    QLabel *lblPath = new QLabel(tr("输出路径:"), this);
    m_editOutputPath = new QLineEdit(this);
    
    // 设置默认输出路径
    QString defaultPath = QCoreApplication::applicationDirPath() + "/test.dat";
    m_editOutputPath->setText(defaultPath);
    m_editOutputPath->setPlaceholderText(tr("请选择输出文件路径"));
    m_editOutputPath->setToolTip(tr("数据文件的输出路径"));
    
    m_btnBrowse = new QPushButton(tr("浏览..."), this);
    m_btnBrowse->setToolTip(tr("选择输出文件位置"));
    m_btnBrowse->setFixedWidth(80);

    layout->addWidget(lblPath);
    layout->addWidget(m_editOutputPath, 1);
    layout->addWidget(m_btnBrowse);

    connect(m_btnBrowse, &QPushButton::clicked,
            this, &MainWindow::browseOutputPath);

    return group;
}

QGroupBox* MainWindow::createProgressGroup()
{
    QGroupBox *group = new QGroupBox(tr("生成进度"), this);
    QVBoxLayout *layout = new QVBoxLayout(group);
    layout->setSpacing(10);

    // 进度条
    m_progressBar = new QProgressBar(this);
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_progressBar->setTextVisible(true);
    m_progressBar->setFormat(tr("%p%"));
    m_progressBar->setFixedHeight(25);
    layout->addWidget(m_progressBar);

    // 状态标签
    m_lblStatus = new QLabel(tr("就绪"), this);
    m_lblStatus->setWordWrap(true);
    layout->addWidget(m_lblStatus);

    // 预估信息标签
    m_lblEstimate = new QLabel(this);
    m_lblEstimate->setObjectName("estimateLabel");
    m_lblEstimate->setWordWrap(true);
    layout->addWidget(m_lblEstimate);

    return group;
}

QWidget* MainWindow::createButtonPanel()
{
    QWidget *panel = new QWidget(this);
    QHBoxLayout *layout = new QHBoxLayout(panel);
    layout->setSpacing(20);
    layout->setContentsMargins(0, 10, 0, 0);

    layout->addStretch();

    // 生成按钮
    m_btnGenerate = new QPushButton(tr("开始生成"), this);
    m_btnGenerate->setObjectName("primaryButton");
    m_btnGenerate->setFixedSize(120, 40);
    m_btnGenerate->setToolTip(tr("开始生成三维数据"));
    layout->addWidget(m_btnGenerate);

    // 取消按钮
    m_btnCancel = new QPushButton(tr("取消"), this);
    m_btnCancel->setFixedSize(100, 40);
    m_btnCancel->setEnabled(false);
    m_btnCancel->setToolTip(tr("取消当前生成任务"));
    layout->addWidget(m_btnCancel);

    layout->addStretch();

    connect(m_btnGenerate, &QPushButton::clicked,
            this, &MainWindow::startGeneration);
    connect(m_btnCancel, &QPushButton::clicked,
            this, &MainWindow::cancelGeneration);

    return panel;
}

void MainWindow::applyStyleSheet()
{
    QString style = R"(
        QMainWindow {
            background-color: #f5f7fa;
        }
        
        QLabel {
            color: #333333;
            font-size: 13px;
        }
        
        #titleLabel {
            font-size: 24px;
            font-weight: bold;
            color: #2c3e50;
            padding: 10px;
        }
        
        QGroupBox {
            font-weight: bold;
            font-size: 14px;
            color: #2c3e50;
            border: 2px solid #dcdfe6;
            border-radius: 8px;
            margin-top: 12px;
            padding-top: 10px;
            background-color: #ffffff;
        }
        
        QGroupBox::title {
            subcontrol-origin: margin;
            subcontrol-position: top left;
            padding: 0 10px;
            background-color: #ffffff;
        }
        
        QSpinBox, QDoubleSpinBox, QLineEdit {
            padding: 8px;
            border: 1px solid #dcdfe6;
            border-radius: 4px;
            background-color: #ffffff;
            font-size: 13px;
            min-height: 20px;
        }
        
        QSpinBox:focus, QDoubleSpinBox:focus, QLineEdit:focus {
            border-color: #409eff;
            outline: none;
        }
        
        QSpinBox:hover, QDoubleSpinBox:hover, QLineEdit:hover {
            border-color: #c0c4cc;
        }
        
        QPushButton {
            padding: 8px 16px;
            border: none;
            border-radius: 4px;
            background-color: #409eff;
            color: white;
            font-size: 14px;
            font-weight: bold;
        }
        
        QPushButton:hover {
            background-color: #66b1ff;
        }
        
        QPushButton:pressed {
            background-color: #3a8ee6;
        }
        
        QPushButton:disabled {
            background-color: #a0cfff;
            color: #ffffff;
        }
        
        #primaryButton {
            background-color: #67c23a;
        }
        
        #primaryButton:hover {
            background-color: #85ce61;
        }
        
        #primaryButton:pressed {
            background-color: #5daf34;
        }
        
        #primaryButton:disabled {
            background-color: #b3e19d;
        }
        
        QProgressBar {
            border: 1px solid #dcdfe6;
            border-radius: 4px;
            background-color: #ebeef5;
            text-align: center;
            font-weight: bold;
            color: #606266;
        }
        
        QProgressBar::chunk {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                stop:0 #67c23a, stop:1 #85ce61);
            border-radius: 3px;
        }
        
        #estimateLabel {
            color: #909399;
            font-size: 12px;
            font-style: italic;
        }
    )";
    
    setStyleSheet(style);
}

void MainWindow::setControlsEnabled(bool enabled)
{
    m_spinDimX->setEnabled(enabled);
    m_spinDimY->setEnabled(enabled);
    m_spinDimZ->setEnabled(enabled);
    m_spinMinValue->setEnabled(enabled);
    m_spinMaxValue->setEnabled(enabled);
    m_editOutputPath->setEnabled(enabled);
    m_btnBrowse->setEnabled(enabled);
    m_btnGenerate->setEnabled(enabled);
    m_btnCancel->setEnabled(!enabled);
}

void MainWindow::browseOutputPath()
{
    QString currentPath = m_editOutputPath->text();
    QString dir = QFileInfo(currentPath).absolutePath();
    
    QString filePath = QFileDialog::getSaveFileName(
        this,
        tr("选择输出文件"),
        currentPath.isEmpty() ? dir : currentPath,
        tr("二进制文件 (*.bin);;所有文件 (*.*)")
    );
    
    if (!filePath.isEmpty()) {
        m_editOutputPath->setText(filePath);
    }
}

void MainWindow::startGeneration()
{
    // 验证输入
    if (m_spinMinValue->value() >= m_spinMaxValue->value()) {
        QMessageBox::warning(this, tr("参数错误"),
            tr("最小值必须小于最大值！"));
        return;
    }

    QString outputPath = m_editOutputPath->text().trimmed();
    if (outputPath.isEmpty()) {
        QMessageBox::warning(this, tr("参数错误"),
            tr("请指定输出文件路径！"));
        return;
    }

    // 设置生成器参数
    m_generator->setDimensions(
        m_spinDimX->value(),
        m_spinDimY->value(),
        m_spinDimZ->value()
    );
    m_generator->setOutputPath(outputPath);
    m_generator->setValueRange(
        static_cast<float>(m_spinMinValue->value()),
        static_cast<float>(m_spinMaxValue->value())
    );

    // 禁用控件
    setControlsEnabled(false);
    m_progressBar->setValue(0);
    m_lblStatus->setText(tr("正在生成数据..."));

    // 创建工作线程
    m_workerThread = QThread::create([this]() {
        m_generator->startGeneration();
    });
    
    connect(m_workerThread, &QThread::finished, this, [this]() {
        m_workerThread->deleteLater();
        m_workerThread = nullptr;
    });

    // 连接信号
    connect(m_generator.get(), &DataGenerator::progressUpdated,
            this, &MainWindow::updateProgress, Qt::QueuedConnection);
    connect(m_generator.get(), &DataGenerator::generationFinished,
            this, &MainWindow::onGenerationFinished, Qt::QueuedConnection);

    m_workerThread->start();
}

void MainWindow::cancelGeneration()
{
    if (m_generator) {
        m_generator->stopGeneration();
        m_lblStatus->setText(tr("正在取消..."));
    }
}

void MainWindow::updateProgress(int percent, const QString &message)
{
    m_progressBar->setValue(percent);
    m_lblStatus->setText(message);
}

void MainWindow::onGenerationFinished(bool success, const QString &message)
{
    setControlsEnabled(true);
    m_progressBar->setValue(success ? 100 : m_progressBar->value());
    m_lblStatus->setText(message);

    if (success) {
        QMessageBox::information(this, tr("生成完成"), message);
    } else {
        QMessageBox::warning(this, tr("生成失败"), message);
    }
}

void MainWindow::updateEstimateInfo()
{
    m_generator->setDimensions(
        m_spinDimX->value(),
        m_spinDimY->value(),
        m_spinDimZ->value()
    );

    size_t totalPoints = m_generator->getTotalDataPoints();
    size_t fileSize = m_generator->getEstimatedFileSize();

    // 格式化数字
    auto formatNumber = [](size_t num) -> QString {
        QString str = QString::number(num);
        int pos = str.length() - 3;
        while (pos > 0) {
            str.insert(pos, ',');
            pos -= 3;
        }
        return str;
    };

    // 格式化文件大小
    auto formatSize = [](size_t bytes) -> QString {
        const char* units[] = {"B", "KB", "MB", "GB", "TB"};
        int unitIndex = 0;
        double size = static_cast<double>(bytes);
        while (size >= 1024.0 && unitIndex < 4) {
            size /= 1024.0;
            unitIndex++;
        }
        return QString("%1 %2").arg(size, 0, 'f', 2).arg(units[unitIndex]);
    };

    m_lblEstimate->setText(tr("预估数据点数: %1 | 预估文件大小: %2")
        .arg(formatNumber(totalPoints))
        .arg(formatSize(fileSize)));
}
