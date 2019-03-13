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
    std::string ListenAddr;
    uint16_t ListenPort;
};

//////////////////////////////////////////////////////////////////////////////// Server

class Server
{
    struct Session
    {
        TcpSocket Socket;
        Time::Tick LastAlive;
        bool Dead;

        Session(TcpSocket&& socket)
            : Socket(std::move(socket)), LastAlive(0), Dead(true) {}

        void OnTcpError(int error)
        {
            MOE_LOG_ERROR("Socket {0} error: {1}", Socket.GetPeerName(), error);
            Dead = true;
        }

        void OnTcpData(BytesView data)
        {
            LastAlive = RunLoop::Now();
            Socket.Write(data);
        }

        void OnTcpDataEof()
        {
            MOE_LOG_ERROR("Remote {0} close socket", Socket.GetPeerName());
            Socket.Close();
            Dead = true;
        }
    };

public:
    Server(const Configure& cfg)
        : m_stConfig(cfg), m_stRunLoop(m_stObjectPool), m_stTimer(Timer::CreateTickTimer(1000)), m_stTcpSocket(TcpSocket::Create()),
        m_stUdpSocket(UdpSocket::Create())
    {
        m_stTimer.SetOnTimeCallback(bind(&Server::OnTick, this));

        m_stTcpSocket.SetOnConnectionCallback(bind(&Server::OnTcpConnection, this));
        m_stTcpSocket.SetOnErrorCallback(bind(&Server::OnTcpError, this, placeholders::_1));

        m_stUdpSocket.SetOnDataCallback(bind(&Server::OnUdpData, this, placeholders::_1, placeholders::_2));
        m_stUdpSocket.SetOnErrorCallback(bind(&Server::OnUdpError, this, placeholders::_1));

        m_stTcpSocket.Bind(EndPoint(m_stConfig.ListenAddr, m_stConfig.ListenPort), false);
        m_stUdpSocket.Bind(EndPoint(m_stConfig.ListenAddr, m_stConfig.ListenPort));
    }

public:
    void Run()
    {
        m_stTimer.Start();
        m_stTcpSocket.Listen();
        m_stUdpSocket.StartRead();

        m_stRunLoop.Run();
    }

protected:
    void OnTick()
    {
        auto now = m_stRunLoop.Now();
        for (auto it = m_stSessions.begin(); it != m_stSessions.end(); )
        {
            if ((*it)->Dead)
            {
                it = m_stSessions.erase(it);
                continue;
            }

            if ((*it)->LastAlive + 60 * 1000 <= now)
            {
                (*it)->Socket.Close();
                (*it)->Dead = true;
            }
            ++it;
        }
    }

    void OnTcpConnection()
    {
        auto session = make_shared<Session>(m_stTcpSocket.Accept());
        MOE_LOG_INFO("Accept session from {0}, current session count {1}", session->Socket.GetPeerName(), m_stSessions.size() + 1);

        session->LastAlive = RunLoop::Now();
        session->Dead = false;

        session->Socket.SetOnErrorCallback(bind(&Session::OnTcpError, session.get(), placeholders::_1));
        session->Socket.SetOnDataCallback(bind(&Session::OnTcpData, session.get(), placeholders::_1));
        session->Socket.SetOnEofCallback(bind(&Session::OnTcpDataEof, session.get()));
        session->Socket.StartRead();

        m_stSessions.emplace_back(session);
    }

    void OnTcpError(int err)
    {
        MOE_LOG_FATAL("Server tcp socket error: {0}", err);
        ::abort();
    }

    void OnUdpData(const EndPoint& from, BytesView data)
    {
        m_stUdpSocket.Send(from, data);
    }

    void OnUdpError(int err)
    {
        MOE_LOG_FATAL("Server udp socket error: {0}", err);
        ::abort();
    }

private:
    Configure m_stConfig;

    ObjectPool m_stObjectPool;
    RunLoop m_stRunLoop;
    Timer m_stTimer;
    TcpSocket m_stTcpSocket;
    UdpSocket m_stUdpSocket;

    std::vector<std::shared_ptr<Session>> m_stSessions;
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
    parser << CmdParser::Option(cfg.ListenAddr, "listen", 'l', "Specific the server listen ip address", string("0.0.0.0"));
    parser << CmdParser::Option(cfg.ListenPort, "port", 'p', "Specific the server listen port (TCP & UDP)");
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
        Server server(cfg);
        server.Run();
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
