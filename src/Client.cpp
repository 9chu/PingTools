#include <deque>

#include <Moe.Core/Logging.hpp>
#include <Moe.Core/CmdParser.hpp>
#include <Moe.Core/Mdr.hpp>

#include <Moe.UV/Timer.hpp>
#include <Moe.UV/RunLoop.hpp>
#include <Moe.UV/TcpSocket.hpp>
#include <Moe.UV/UdpSocket.hpp>

using namespace std;
using namespace moe;
using namespace UV;

struct Configure
{
    std::string ServerAddr;
    uint16_t ServerPort;
    uint32_t PingInterval;
    uint32_t PingTimeout;
    std::string Output;
};

struct PingPacket
{
    MOE_DR_FIELDS(
        (0, uint32_t, Seq),
        (1, Time::Tick, SendTime)
    )
};

//////////////////////////////////////////////////////////////////////////////// PingStatistic

struct PingStatistic
{
    uint32_t TotalPacket;
    uint32_t PacketLost;
    uint32_t AvailablePacket;
    uint32_t LatencyTotal;
    uint32_t MaxLatency;
    uint32_t MinLatency;
};

class Pinger
{
public:
    Pinger(uint32_t interval, uint32_t timeout)
        : m_uInterval(interval), m_uTimeout(timeout) {}
    
public:
    Optional<PingPacket> Update(Time::Tick now)
    {
        Optional<PingPacket> ret;
        if (m_ullNextSendTime <= now)
        {
            // 处理超时
            while (!m_stPingWindow.empty() && m_stPingWindow.size() * m_uInterval >= m_uTimeout)
            {
                if (!m_stPingWindow[0])
                    ++m_uPacketLost;
                m_stPingWindow.pop_front();
            }

            // 发送PING包
            PingPacket packet {};
            packet.Seq = m_uNextSeq++;
            packet.SendTime = now;

            m_stPingWindow.emplace_back(false);

            m_uTotalPacket += 1;
            m_ullNextSendTime = now + m_uInterval;
            ret = packet;
        }
        return ret;
    }

    void Recv(const PingPacket& packet)
    {
        auto offset = m_stPingWindow.size() - (m_uNextSeq - packet.Seq);
        if (offset >= m_stPingWindow.size())
            return;
        if (m_stPingWindow[offset])
            return;

        auto now = RunLoop::Now();
        auto elapsed = static_cast<uint32_t>(now - packet.SendTime);

        m_stPingWindow[offset] = true;
        m_uAvailablePacket += 1;
        m_uLatencyTotal += elapsed;
        m_uMaxLatency = std::max(m_uMaxLatency, elapsed);
        m_uMinLatency = std::min(m_uMinLatency, elapsed);
    }

    PingStatistic GetStatistic()
    {
        PingStatistic desc {};
        desc.TotalPacket = m_uTotalPacket;
        desc.PacketLost = m_uPacketLost;
        desc.AvailablePacket = m_uAvailablePacket;
        desc.LatencyTotal = m_uLatencyTotal;
        desc.MaxLatency = m_uAvailablePacket == 0 ? 0 : m_uMaxLatency;
        desc.MinLatency = m_uAvailablePacket == 0 ? 0 : m_uMinLatency;
        return desc;
    }

    void Reset()
    {
        m_stPingWindow.clear();

        m_ullNextSendTime = 0;
        m_uTotalPacket = 0;
        m_uPacketLost = 0;
        m_uAvailablePacket = 0;
        m_uLatencyTotal = 0;
        m_uMaxLatency = 0;
        m_uMinLatency = numeric_limits<uint32_t>::max();
    }

private:
    const uint32_t m_uInterval = 0;
    const uint32_t m_uTimeout = 0;

    Time::Tick m_ullNextSendTime = 0;  // 下一次发送时间
    uint32_t m_uNextSeq = 0;  // 下一个要发还未发的Seq
    std::deque<bool> m_stPingWindow;

    uint32_t m_uTotalPacket = 0;  // 总包量
    uint32_t m_uPacketLost = 0;  // 丢包量
    uint32_t m_uAvailablePacket = 0;  // 有效采样包
    uint32_t m_uLatencyTotal = 0;  // 总时延
    uint32_t m_uMaxLatency = 0;  // 最大时延
    uint32_t m_uMinLatency = numeric_limits<uint32_t>::max();  // 最小时延
};

//////////////////////////////////////////////////////////////////////////////// Client

class Client
{
    enum {
        STATE_TCP_NOT_CONNECT = 0,
        STATE_TCP_CONNECTING = 1,
        STATE_TCP_CONNECTED = 2,
    };

public:
    Client(const Configure& cfg)
        : m_stConfig(cfg), m_stServerEndPoint(cfg.ServerAddr, cfg.ServerPort), m_stRunLoop(m_stObjectPool),
        m_stTimer(Timer::CreateTickTimer(100)), m_stTcpSocket(TcpSocket::Create()), m_stUdpSocket(UdpSocket::Create()),
        m_stTcpPinger(cfg.PingInterval, cfg.PingTimeout), m_stUdpPinger(cfg.PingInterval, cfg.PingTimeout)
    {
        m_stTimer.SetOnTimeCallback(bind(&Client::OnTick, this));

        BindTcpEvent();
        BindUdpEvent();

        if (!cfg.Output.empty())
        {
            auto formatter = make_shared<Logging::PlainFormatter>();
            formatter->SetFormat("{short_date} {time}|{msg}");

            m_pSink = make_shared<Logging::RotatingFileSink>(cfg.Output.c_str());
            m_pSink->SetMinLevel(Logging::Level::Debug);
            m_pSink->SetMaxLevel(Logging::Level::Info);
            m_pSink->SetFormatter(formatter);
        }
    }

public:
    void Run()
    {
        m_stTimer.Start();
        m_stUdpSocket.StartRead();

        m_stRunLoop.Run();
    }

protected:
    void BindTcpEvent()
    {
        m_stTcpSocket.SetOnConnectCallback(bind(&Client::OnTcpConnected, this, placeholders::_1));
        m_stTcpSocket.SetOnErrorCallback(bind(&Client::OnTcpError, this, placeholders::_1));
        m_stTcpSocket.SetOnDataCallback(bind(&Client::OnTcpData, this, placeholders::_1));
        m_stTcpSocket.SetOnEofCallback(bind(&Client::OnTcpDataEof, this));
    }

    void BindUdpEvent()
    {
        m_stUdpSocket.SetOnDataCallback(bind(&Client::OnUdpData, this, placeholders::_1, placeholders::_2));
        m_stUdpSocket.SetOnErrorCallback(bind(&Client::OnUdpError, this, placeholders::_1));
    }

    void OnTick()
    {
        auto now = m_stRunLoop.Now();
        if (m_iTcpChannelState == STATE_TCP_NOT_CONNECT && now >= m_ullNextTryConnectTime)
        {
            m_stTcpSocket.Connect(m_stServerEndPoint);
            m_iTcpChannelState = STATE_TCP_CONNECTING;
        }

        auto tcpPacket = m_stTcpPinger.Update(now);
        auto udpPacket = m_stUdpPinger.Update(now);

        if (m_iTcpChannelState == STATE_TCP_CONNECTED && tcpPacket)
        {
            m_stBuffer.clear();
            Mdr::WriteStruct(*tcpPacket, m_stBuffer);
            m_stTcpSocket.Write(ToArrayView<uint8_t>(m_stBuffer));
        }

        if (udpPacket)
        {
            m_stBuffer.clear();
            Mdr::WriteStruct(*udpPacket, m_stBuffer);
            m_stUdpSocket.Send(m_stServerEndPoint, ToArrayView<uint8_t>(m_stBuffer));
        }

        if (now >= m_ullNextPintStatTime)
        {
            m_ullNextPintStatTime = now + 60 * 1000;

            auto tcpStat = m_stTcpPinger.GetStatistic();
            auto udpStat = m_stUdpPinger.GetStatistic();

            auto total = tcpStat.PacketLost + tcpStat.AvailablePacket;
            MOE_LOG_INFO("TCP PING, Packet loss {0}/{1} ({2:F2}%), avg {3:F2}ms, max {4}ms, min {5}ms", tcpStat.PacketLost,
                total, total == 0 ? 0 : 100. * tcpStat.PacketLost / total,
                tcpStat.AvailablePacket == 0 ? 0 : static_cast<double>(tcpStat.LatencyTotal / tcpStat.AvailablePacket),
                tcpStat.MaxLatency, tcpStat.MinLatency);
            if (m_pSink)
            {
                m_pSink->Log(Logging::Level::Info, Logging::Context(__FILE__, __LINE__, __FUNCTION__), StringUtils::Format(
                    "TCP|{0}|{1}|{2:F2}%|{3:F2}|{4}|{5}", tcpStat.PacketLost, total, total == 0 ? 0 : 100. * tcpStat.PacketLost / total,
                    tcpStat.AvailablePacket == 0 ? 0 : static_cast<double>(tcpStat.LatencyTotal / tcpStat.AvailablePacket),
                    tcpStat.MaxLatency, tcpStat.MinLatency).c_str());
            }

            total = udpStat.PacketLost + udpStat.AvailablePacket;
            MOE_LOG_INFO("UDP PING, Packet loss {0}/{1} ({2:F2}%), avg {3:F2}ms, max {4}ms, min {5}ms", udpStat.PacketLost,
                total, total == 0 ? 0 : 100. * udpStat.PacketLost / total,
                udpStat.AvailablePacket == 0 ? 0 : static_cast<double>(udpStat.LatencyTotal / udpStat.AvailablePacket),
                udpStat.MaxLatency, udpStat.MinLatency);
            if (m_pSink)
            {
                m_pSink->Log(Logging::Level::Info, Logging::Context(__FILE__, __LINE__, __FUNCTION__), StringUtils::Format(
                    "UDP|{0}|{1}|{2:F2}%|{3:F2}|{4}|{5}", udpStat.PacketLost, total, total == 0 ? 0 : 100. * udpStat.PacketLost / total,
                    udpStat.AvailablePacket == 0 ? 0 : static_cast<double>(udpStat.LatencyTotal / udpStat.AvailablePacket),
                    udpStat.MaxLatency, udpStat.MinLatency).c_str());
            }

            m_stTcpPinger.Reset();
            m_stUdpPinger.Reset();
        }
    }

    void OnTcpConnected(int err)
    {
        if (err == 0)
        {
            MOE_LOG_INFO("Ping server connected");
            m_iTcpChannelState = STATE_TCP_CONNECTED;
            m_stTcpPinger.Reset();

            m_stTcpSocket.StartRead();
        }
        else
        {
            MOE_LOG_ERROR("Connect failed, err {0}", err);
            m_ullNextTryConnectTime = RunLoop::Now() + 10 * 1000;
            m_iTcpChannelState = STATE_TCP_NOT_CONNECT;
        }
    }

    void OnTcpError(int err)
    {
        MOE_LOG_ERROR("Tcp socket error: {0}", err);

        // 创建新的Socket
        m_stTcpSocket = TcpSocket::Create();
        m_iTcpChannelState = STATE_TCP_NOT_CONNECT;
        m_ullNextTryConnectTime = RunLoop::Now() + 10 * 1000;
        BindTcpEvent();
    }

    void OnTcpData(BytesView data)
    {
        try
        {
            PingPacket packet {};
            Mdr::ReadStruct(packet, data);
            m_stTcpPinger.Recv(packet);
        }
        catch (const ExceptionBase& ex)
        {
            MOE_LOG_EXCEPTION(ex);
        }
    }

    void OnTcpDataEof()
    {
        MOE_LOG_ERROR("Tcp socket: remote EOF");
        m_stTcpSocket.Close();

        // 创建新的Socket
        m_stTcpSocket = TcpSocket::Create();
        m_iTcpChannelState = STATE_TCP_NOT_CONNECT;
        m_ullNextTryConnectTime = RunLoop::Now() + 10 * 1000;
        BindTcpEvent();
    }

    void OnUdpData(const EndPoint&, BytesView data)
    {
        try
        {
            PingPacket packet {};
            Mdr::ReadStruct(packet, data);
            m_stUdpPinger.Recv(packet);
        }
        catch (const ExceptionBase& ex)
        {
            MOE_LOG_EXCEPTION(ex);
        }
    }

    void OnUdpError(int err)
    {
        MOE_LOG_ERROR("Udp socket error: {0}", err);

        // 创建新的Socket
        m_stUdpSocket = UdpSocket::Create();
        BindUdpEvent();
        m_stUdpSocket.StartRead();
    }

private:
    Configure m_stConfig;
    EndPoint m_stServerEndPoint;

    ObjectPool m_stObjectPool;
    RunLoop m_stRunLoop;
    Timer m_stTimer;
    TcpSocket m_stTcpSocket;
    UdpSocket m_stUdpSocket;

    int m_iTcpChannelState = STATE_TCP_NOT_CONNECT;
    Time::Tick m_ullNextTryConnectTime = 0;

    Time::Tick m_ullNextPintStatTime = 0;

    Pinger m_stTcpPinger;
    Pinger m_stUdpPinger;
    vector<uint8_t> m_stBuffer;

    std::shared_ptr<Logging::RotatingFileSink> m_pSink;
};

//////////////////////////////////////////////////////////////////////////////// App

static void InitLogger()
{
    auto& logger = Logging::GetInstance();

    // 默认调试输出（控制台）
    auto formatter = make_shared<Logging::PlainFormatter>();
    auto stdoutLogger = make_shared<Logging::TerminalSink>(Logging::TerminalSink::OutputType::StdOut);
    stdoutLogger->SetMinLevel(Logging::Level::Debug);
    stdoutLogger->SetMaxLevel(Logging::Level::Info);
    stdoutLogger->SetFormatter(formatter);
    logger.AppendSink(stdoutLogger);

    auto formatter2 = make_shared<Logging::PlainFormatter>();
    auto stderrLogger = make_shared<Logging::TerminalSink>(Logging::TerminalSink::OutputType::StdErr);
    stderrLogger->SetMinLevel(Logging::Level::Warn);
    stderrLogger->SetMaxLevel(Logging::Level::Fatal);
    stderrLogger->SetFormatter(formatter2);
    logger.AppendSink(stderrLogger);

    // 设置调试级别
    logger.SetMinLevel(Logging::Level::Debug);

    // 提交改动
    logger.Commit();
}

static Configure ParseCommandline(int argc, const char* argv[])
{
    Configure cfg;
    bool needHelp = false;

    CmdParser parser;
    parser << CmdParser::Option(cfg.ServerAddr, "server", 's', "Specific the server ip address");
    parser << CmdParser::Option(cfg.ServerPort, "port", 'p', "Specific the server port");
    parser << CmdParser::Option(cfg.PingInterval, "interval", 'i', "Specific the ping interval", 1000u);
    parser << CmdParser::Option(cfg.PingTimeout, "timeout", 't', "Specific the ping timeout", 10000u);
    parser << CmdParser::Option(cfg.Output, "output", 'o', "Specific the stat rolling output file", string());
    parser << CmdParser::Option(needHelp, "help", 'h', "Show this help", false);

    try
    {
        parser(argc, argv);
    }
    catch (const ExceptionBase& ex)
    {
        fprintf(stderr, "%s\n\n", ex.GetDescription().c_str());
        needHelp = true;
    }

    if (needHelp)
    {
        auto name = PathUtils::GetFileName(argv[0]);
        auto nameStr = string(name.GetBuffer(), name.GetSize());

        fprintf(stderr, "%s\n", parser.BuildUsageText(nameStr.c_str()).c_str());
        fprintf(stderr, "%s\n", parser.BuildOptionsText(2, 10).c_str());
        exit(1);
    }

    InitLogger();
    return std::move(cfg);
}

int main(int argc, const char* argv[])
{
    try
    {
        Configure cfg = ParseCommandline(argc, argv);
        Client client(cfg);
        client.Run();
    }
    catch (const moe::ExceptionBase& ex)
    {
        MOE_LOG_EXCEPTION(ex);
        return -1;
    }
    catch (const std::exception& ex)
    {
        MOE_LOG_FATAL("Unhandled exception: {0}", ex.what());
        return -1;
    }
    return 0;
}
