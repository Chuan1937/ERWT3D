#ifndef DATA_GENERATOR_H
#define DATA_GENERATOR_H

#include <QObject>
#include <QString>
#include <QFile>
#include <QDataStream>
#include <QProgressBar>
#include <atomic>
#include <random>
#include <functional>

/**
 * @brief 三维浮点数据生成器类
 * 
 * 该类负责生成三维浮点数据集并写入本地文件。
 * 数据按X-Y-Z三个维度的顺序存储。
 */
class DataGenerator : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief 构造函数
     * @param parent 父对象指针
     */
    explicit DataGenerator(QObject *parent = nullptr);
    
    /**
     * @brief 析构函数
     */
    ~DataGenerator() override = default;

    /**
     * @brief 设置数据维度大小
     * @param dimX X维度大小
     * @param dimY Y维度大小
     * @param dimZ Z维度大小
     */
    void setDimensions(size_t dimX, size_t dimY, size_t dimZ);

    /**
     * @brief 设置输出文件路径
     * @param filePath 文件完整路径
     */
    void setOutputPath(const QString &filePath);

    /**
     * @brief 设置随机数范围
     * @param minVal 最小值
     * @param maxVal 最大值
     */
    void setValueRange(float minVal, float maxVal);

    /**
     * @brief 获取总数据点数量
     * @return 总数据点数量
     */
    size_t getTotalDataPoints() const;

    /**
     * @brief 获取预计文件大小（字节）
     * @return 预计文件大小
     */
    size_t getEstimatedFileSize() const;

    /**
     * @brief 停止生成过程
     */
    void stopGeneration();

public slots:
    /**
     * @brief 开始生成数据
     */
    void startGeneration();

signals:
    /**
     * @brief 进度更新信号
     * @param percent 完成百分比（0-100）
     * @param message 状态消息
     */
    void progressUpdated(int percent, const QString &message);

    /**
     * @brief 生成完成信号
     * @param success 是否成功
     * @param message 结果消息
     */
    void generationFinished(bool success, const QString &message);

    /**
     * @brief 错误发生信号
     * @param errorMessage 错误消息
     */
    void errorOccurred(const QString &errorMessage);

private:
    /**
     * @brief 生成单个随机浮点数
     * @return 随机浮点数
     */
    float generateRandomFloat();

    /**
     * @brief 写入文件头信息
     * @param file 文件对象
     * @return 是否成功
     */
    bool writeFileHeader(QFile &file);

    /**
     * @brief 写入数据内容
     * @param file 文件对象
     * @return 是否成功
     */
    bool writeDataContent(QFile &file);

    /**
     * @brief 格式化文件大小显示
     * @param bytes 字节数
     * @return 格式化后的字符串
     */
    QString formatFileSize(size_t bytes) const;

    /**
     * @brief 格式化数字显示（添加千位分隔符）
     * @param num 数字
     * @return 格式化后的字符串
     */
    QString formatNumber(size_t num) const;

private:
    size_t m_dimX;              ///< X维度大小
    size_t m_dimY;              ///< Y维度大小
    size_t m_dimZ;              ///< Z维度大小
    QString m_outputPath;       ///< 输出文件路径
    float m_minValue;           ///< 随机数最小值
    float m_maxValue;           ///< 随机数最大值
    std::atomic<bool> m_stopFlag; ///< 停止标志
    
    std::mt19937 m_generator;   ///< 随机数生成器
    std::uniform_real_distribution<float> m_distribution; ///< 均匀分布
};

#endif // DATA_GENERATOR_H
