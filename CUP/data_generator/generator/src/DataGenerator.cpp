#include "DataGenerator.h"
#include <QFileInfo>
#include <QDir>
#include <QDateTime>
#include <QDebug>
#include <vector>

DataGenerator::DataGenerator(QObject *parent)
    : QObject(parent)
    , m_dimX(100)
    , m_dimY(100)
    , m_dimZ(100)
    , m_outputPath("")
    , m_minValue(0.0f)
    , m_maxValue(1.0f)
    , m_stopFlag(false)
{
    // 使用当前时间作为随机种子
    m_generator.seed(static_cast<unsigned int>(
        std::chrono::system_clock::now().time_since_epoch().count()
    ));
    m_distribution = std::uniform_real_distribution<float>(m_minValue, m_maxValue);
}

void DataGenerator::setDimensions(size_t dimX, size_t dimY, size_t dimZ)
{
    m_dimX = dimX;
    m_dimY = dimY;
    m_dimZ = dimZ;
}

void DataGenerator::setOutputPath(const QString &filePath)
{
    m_outputPath = filePath;
}

void DataGenerator::setValueRange(float minVal, float maxVal)
{
    m_minValue = minVal;
    m_maxValue = maxVal;
    m_distribution = std::uniform_real_distribution<float>(m_minValue, m_maxValue);
}

size_t DataGenerator::getTotalDataPoints() const
{
    return m_dimX * m_dimY * m_dimZ;
}

size_t DataGenerator::getEstimatedFileSize() const
{
    // 每个浮点数4字节 + 文件头（约100字节）
    return getTotalDataPoints() * sizeof(float) + 100;
}

void DataGenerator::stopGeneration()
{
    m_stopFlag = true;
}

void DataGenerator::startGeneration()
{
    m_stopFlag = false;
    
    // 验证参数
    if (m_dimX == 0 || m_dimY == 0 || m_dimZ == 0) {
        emit errorOccurred(tr("维度大小不能为零！"));
        emit generationFinished(false, tr("参数错误"));
        return;
    }

    if (m_outputPath.isEmpty()) {
        emit errorOccurred(tr("输出路径不能为空！"));
        emit generationFinished(false, tr("参数错误"));
        return;
    }

    // 确保输出目录存在
    QFileInfo fileInfo(m_outputPath);
    QDir dir = fileInfo.absoluteDir();
    if (!dir.exists()) {
        if (!dir.mkpath(".")) {
            emit errorOccurred(tr("无法创建输出目录！"));
            emit generationFinished(false, tr("目录创建失败"));
            return;
        }
    }

    emit progressUpdated(0, tr("正在准备生成数据..."));

    // 打开文件
    QFile file(m_outputPath);
    if (!file.open(QIODevice::WriteOnly)) {
        emit errorOccurred(tr("无法打开输出文件：%1").arg(file.errorString()));
        emit generationFinished(false, tr("文件打开失败"));
        return;
    }

    // 写入数据内容
    bool success = writeDataContent(file);
    file.close();

    if (m_stopFlag) {
        emit generationFinished(false, tr("用户取消操作"));
    } else if (success) {
        QString sizeInfo = formatFileSize(getEstimatedFileSize());
        emit generationFinished(true, tr("数据生成完成！\n文件大小：%1\n数据点数：%2")
            .arg(sizeInfo)
            .arg(getTotalDataPoints()));
    } else {
        emit generationFinished(false, tr("数据写入失败"));
    }
}

float DataGenerator::generateRandomFloat()
{
    return m_distribution(m_generator);
}

bool DataGenerator::writeFileHeader(QFile &file)
{
    QDataStream out(&file);
    out.setByteOrder(QDataStream::LittleEndian);
    out.setFloatingPointPrecision(QDataStream::SinglePrecision);

    // 写入文件标识（魔数）
    out.writeRawData("3DDF", 4);  // 3D Data File

    // 写入版本号
    quint32 version = 1;
    out << version;

    // 写入维度信息
    out << static_cast<quint64>(m_dimX);
    out << static_cast<quint64>(m_dimY);
    out << static_cast<quint64>(m_dimZ);

    // 写入数值范围
    out << m_minValue;
    out << m_maxValue;

    // 写入时间戳
    quint64 timestamp = static_cast<quint64>(
        QDateTime::currentSecsSinceEpoch()
    );
    out << timestamp;

    return true;
}

bool DataGenerator::writeDataContent(QFile &file)
{
    QDataStream out(&file);
    out.setByteOrder(QDataStream::LittleEndian);
    out.setFloatingPointPrecision(QDataStream::SinglePrecision);

    size_t totalPoints = getTotalDataPoints();
    size_t processedPoints = 0;
    int lastPercent = 0;

    // 缓冲区，用于批量写入提高性能
    constexpr size_t BUFFER_SIZE = 65536;  // 64K个浮点数
    std::vector<float> buffer;
    buffer.reserve(BUFFER_SIZE);

    // 按X-Y-Z顺序写入数据
    for (size_t x = 0; x < m_dimX; ++x) {
        if (m_stopFlag) {
            return false;
        }

        for (size_t y = 0; y < m_dimY; ++y) {
            for (size_t z = 0; z < m_dimZ; ++z) {
                // 生成随机浮点数
                buffer.push_back(generateRandomFloat());
                processedPoints++;

                // 当缓冲区满时写入文件
                if (buffer.size() >= BUFFER_SIZE) {
                    out.writeRawData(reinterpret_cast<const char*>(buffer.data()),
                                   buffer.size() * sizeof(float));
                    buffer.clear();
                }

                // 更新进度（每处理1%更新一次）
                int currentPercent = static_cast<int>(processedPoints * 100 / totalPoints);
                if (currentPercent != lastPercent) {
                    lastPercent = currentPercent;
                    emit progressUpdated(currentPercent,
                        tr("正在生成数据... %1% (%2/%3)")
                            .arg(currentPercent)
                            .arg(formatNumber(processedPoints))
                            .arg(formatNumber(totalPoints)));
                }
            }
        }
    }

    // 写入剩余数据
    if (!buffer.empty()) {
        out.writeRawData(reinterpret_cast<const char*>(buffer.data()),
                        buffer.size() * sizeof(float));
    }

    return true;
}

QString DataGenerator::formatFileSize(size_t bytes) const
{
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int unitIndex = 0;
    double size = static_cast<double>(bytes);

    while (size >= 1024.0 && unitIndex < 4) {
        size /= 1024.0;
        unitIndex++;
    }

    return QString("%1 %2").arg(size, 0, 'f', 2).arg(units[unitIndex]);
}

QString DataGenerator::formatNumber(size_t num) const
{
    QString str = QString::number(num);
    int pos = str.length() - 3;
    while (pos > 0) {
        str.insert(pos, ',');
        pos -= 3;
    }
    return str;
}
