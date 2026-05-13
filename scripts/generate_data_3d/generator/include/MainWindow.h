#ifndef MAIN_WINDOW_H
#define MAIN_WINDOW_H

#include <QMainWindow>
#include <QLabel>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QProgressBar>
#include <QLineEdit>
#include <QGroupBox>
#include <QThread>
#include <memory>

// 前向声明
class DataGenerator;

/**
 * @brief 主窗口类
 * 
 * 提供用户界面，支持设置三维数据维度、输出路径等参数，
 * 并显示生成进度。
 */
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    /**
     * @brief 构造函数
     * @param parent 父窗口指针
     */
    explicit MainWindow(QWidget *parent = nullptr);
    
    /**
     * @brief 析构函数
     */
    ~MainWindow() override;

private slots:
    /**
     * @brief 浏览输出目录
     */
    void browseOutputPath();

    /**
     * @brief 开始生成数据
     */
    void startGeneration();

    /**
     * @brief 取消生成
     */
    void cancelGeneration();

    /**
     * @brief 更新进度显示
     * @param percent 完成百分比
     * @param message 状态消息
     */
    void updateProgress(int percent, const QString &message);

    /**
     * @brief 处理生成完成
     * @param success 是否成功
     * @param message 结果消息
     */
    void onGenerationFinished(bool success, const QString &message);

    /**
     * @brief 更新预估信息
     */
    void updateEstimateInfo();

private:
    /**
     * @brief 初始化UI组件
     */
    void setupUI();

    /**
     * @brief 创建维度设置组
     * @return 组框指针
     */
    QGroupBox* createDimensionGroup();

    /**
     * @brief 创建数值范围设置组
     * @return 组框指针
     */
    QGroupBox* createValueRangeGroup();

    /**
     * @brief 创建输出设置组
     * @return 组框指针
     */
    QGroupBox* createOutputGroup();

    /**
     * @brief 创建进度显示组
     * @return 组框指针
     */
    QGroupBox* createProgressGroup();

    /**
     * @brief 创建按钮组
     * @return 组框指针
     */
    QWidget* createButtonPanel();

    /**
     * @brief 应用样式表
     */
    void applyStyleSheet();

    /**
     * @brief 设置控件启用状态
     * @param enabled 是否启用
     */
    void setControlsEnabled(bool enabled);

private:
    // 维度设置控件
    QSpinBox *m_spinDimX;       ///< X维度输入框
    QSpinBox *m_spinDimY;       ///< Y维度输入框
    QSpinBox *m_spinDimZ;       ///< Z维度输入框

    // 数值范围控件
    QDoubleSpinBox *m_spinMinValue; ///< 最小值输入框
    QDoubleSpinBox *m_spinMaxValue; ///< 最大值输入框

    // 输出设置控件
    QLineEdit *m_editOutputPath;    ///< 输出路径输入框
    QPushButton *m_btnBrowse;       ///< 浏览按钮

    // 进度显示控件
    QProgressBar *m_progressBar;    ///< 进度条
    QLabel *m_lblStatus;            ///< 状态标签
    QLabel *m_lblEstimate;          ///< 预估信息标签

    // 操作按钮
    QPushButton *m_btnGenerate;     ///< 生成按钮
    QPushButton *m_btnCancel;       ///< 取消按钮

    // 数据生成器
    std::unique_ptr<DataGenerator> m_generator; ///< 数据生成器
    QThread *m_workerThread;        ///< 工作线程
};

#endif // MAIN_WINDOW_H
