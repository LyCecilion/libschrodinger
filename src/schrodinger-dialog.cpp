/*
 * schrodinger-dialog.cpp — Qt6 crash dialog helper
 *
 * Launched by libschrodinger.c on a fatal signal. Receives the crash details
 * as argv, renders a Microsoft-style Chinese error dialog, and exits with:
 *   0   -> 确定 (terminate through the original signal)
 *   1   -> 取消 (close immediately and let the parent launch gdb)
 *   125 -> invalid arguments or unusable Qt environment
 *
 * The helper never waits for gdb: its button handler returns immediately.
 */

#include <QApplication>
#include <QCloseEvent>
#include <QColor>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QString>
#include <QVBoxLayout>

#include <cstdint>

namespace
{

enum CrashKind : std::uint8_t
{
    KIND_SEGV_READ = 0,
    KIND_SEGV_WRITTEN,
    KIND_BUS_ALIGNED,
    KIND_BUS_READ,
    KIND_ILL_EXECUTE,
    KIND_FPE_INT_ZERO,
    KIND_FPE_INT_OVERFLOW,
    KIND_FPE_FLT_DIVIDE,
    KIND_FPE_FLT_OVERFLOW,
    KIND_FPE_FLT_UNDERFLOW,
    KIND_FPE_FLT_INVALID,
    KIND_FPE_FLT_SUBSCRIPT,
    KIND_FPE_GENERIC,
    KIND_ABORT,
    KIND_COUNT,
};

constexpr int EXIT_OK = 0;
constexpr int EXIT_DEBUG = 1;
constexpr int EXIT_HELPER_FAILURE = 125;

// Draw the classic Windows error icon (red circle + white X). The theme's
// SP_MessageBoxCritical can render as a "no entry" bar on some platforms, so
// we paint a deterministic 叉号 ourselves.
QPixmap errorIcon(int size)
{
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor::fromRgb(0xc4, 0x2b, 0x1c));
    painter.drawEllipse(0, 0, size, size);

    const int margin = size * 3 / 10;
    QPen pen(Qt::white, size / 12.0);
    pen.setCapStyle(Qt::RoundCap);
    painter.setPen(pen);
    painter.drawLine(margin, margin, size - margin, size - margin);
    painter.drawLine(size - margin, margin, margin, size - margin);
    return pixmap;
}

class CrashDialog : public QDialog
{
  public:
    CrashDialog(const QString &title, const QString &body, bool allowCancel,
                QWidget *parent = nullptr)
        : QDialog(parent)
    {
        setWindowTitle(title);
        setMinimumWidth(720);

        auto *layout = new QVBoxLayout(this);

        auto *top = new QHBoxLayout();
        auto *icon = new QLabel(this);
        icon->setPixmap(errorIcon(32));
        top->addWidget(icon, 0, Qt::AlignTop);

        QString fullBody = body;
        if (allowCancel)
        {
            fullBody +=
                QStringLiteral("\n\n要终止程序，请单击“确定”；\n要调试程序，请单击“取消”。");
        }

        auto *label = new QLabel(fullBody, this);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        label->setWordWrap(true);
        top->addWidget(label, 1);
        layout->addLayout(top);
        auto *buttons = new QDialogButtonBox(this);
        auto *ok = buttons->addButton(QStringLiteral("确定"), QDialogButtonBox::AcceptRole);
        ok->setDefault(true);
        QObject::connect(ok, &QPushButton::clicked, this, [this] { done(EXIT_OK); });

        if (allowCancel)
        {
            auto *cancel = buttons->addButton(QStringLiteral("取消"), QDialogButtonBox::RejectRole);
            QObject::connect(cancel, &QPushButton::clicked, this, [this] { done(EXIT_DEBUG); });
        }
        layout->addWidget(buttons);
    }

  protected:
    // Closing through the window manager follows the safe terminate path,
    // never the debug path.
    void closeEvent(QCloseEvent *event) override
    {
        done(EXIT_OK);
        event->accept();
    }
};

QString applicationErrorTitle(const QString &program)
{
    return program + QStringLiteral(" - 应用程序错误");
}

// Build title + body for one crash category. Returns true on success and
// false only for an unrecognized kind.
bool buildMessage(int kind, const QString &program, const QString &ip, const QString &fault,
                  QString &title, QString &body, bool &allowCancel)
{
    allowCancel = true;

    switch (kind)
    {
    case KIND_SEGV_READ:
        title = applicationErrorTitle(program);
        body = QStringLiteral("\"%1\" 指令引用的 \"%2\" 内存。该内存不能为 read。").arg(ip, fault);
        return true;
    case KIND_SEGV_WRITTEN:
        title = applicationErrorTitle(program);
        body =
            QStringLiteral("\"%1\" 指令引用的 \"%2\" 内存。该内存不能为 written。").arg(ip, fault);
        return true;
    case KIND_BUS_ALIGNED:
        title = applicationErrorTitle(program);
        body =
            QStringLiteral("\"%1\" 指令引用的 \"%2\" 内存。该内存不能为 aligned。").arg(ip, fault);
        return true;
    case KIND_BUS_READ:
        title = applicationErrorTitle(program);
        body = QStringLiteral("\"%1\" 指令引用的 \"%2\" 内存。该内存不能为 read。").arg(ip, fault);
        return true;
    case KIND_ILL_EXECUTE:
        title = applicationErrorTitle(program);
        body =
            QStringLiteral("\"%1\" 指令引用的 \"%2\" 内存。该指令不能为 execute。").arg(ip, fault);
        return true;
    case KIND_FPE_INT_ZERO:
        title = applicationErrorTitle(program);
        body = QStringLiteral("\"%1\" 处发生整数除法。该除数不能为 zero。").arg(ip);
        return true;
    case KIND_FPE_INT_OVERFLOW:
        title = applicationErrorTitle(program);
        body = QStringLiteral("\"%1\" 处发生整数溢出。该整数不能为 overflow。").arg(ip);
        return true;
    case KIND_FPE_FLT_DIVIDE:
        title = applicationErrorTitle(program);
        body = QStringLiteral("\"%1\" 处发生浮点除法。该除数不能为 divide。").arg(ip);
        return true;
    case KIND_FPE_FLT_OVERFLOW:
        title = applicationErrorTitle(program);
        body = QStringLiteral("\"%1\" 处发生浮点溢出。该运算不能为 overflow。").arg(ip);
        return true;
    case KIND_FPE_FLT_UNDERFLOW:
        title = applicationErrorTitle(program);
        body = QStringLiteral("\"%1\" 处发生浮点下溢。该运算不能为 underflow。").arg(ip);
        return true;
    case KIND_FPE_FLT_INVALID:
        title = applicationErrorTitle(program);
        body = QStringLiteral("\"%1\" 处发生无效浮点运算。该运算不能为 invalid。").arg(ip);
        return true;
    case KIND_FPE_FLT_SUBSCRIPT:
        title = applicationErrorTitle(program);
        body = QStringLiteral("\"%1\" 处发生浮点下标越界。该运算不能为 subscript。").arg(ip);
        return true;
    case KIND_FPE_GENERIC:
        title = applicationErrorTitle(program);
        body = QStringLiteral("\"%1\" 处发生算术异常。该运算不能为 execute。").arg(ip);
        return true;
    case KIND_ABORT:
        title = program + QStringLiteral(" - Microsoft Visual C++ Runtime Library");
        body = QStringLiteral("Runtime Error!\n\n"
                              "此应用程序请求运行时以一种异常方式终止它。\n"
                              "请联系应用程序的支持团队以获取更多信息。");
        allowCancel = false;
        return true;
    default:
        return false;
    }
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    // argv: <self> <program> <signal> <si_code> <ip> <fault> <kind>
    if (argc != 7)
    {
        return EXIT_HELPER_FAILURE;
    }

    QString program = QString::fromUtf8(argv[1]);
    if (program.isEmpty())
    {
        program = QStringLiteral("Program");
    }

    bool okSignal = false;
    bool okCode = false;
    bool okKind = false;
    const int signalNumber [[maybe_unused]] = QString::fromUtf8(argv[2]).toInt(&okSignal);
    const int siCode [[maybe_unused]] = QString::fromUtf8(argv[3]).toInt(&okCode);
    const int kind = QString::fromUtf8(argv[6]).toInt(&okKind);

    if (!okSignal || !okCode || !okKind || kind < 0 || kind >= KIND_COUNT)
    {
        return EXIT_HELPER_FAILURE;
    }

    const QString ip = QString::fromUtf8(argv[4]);
    const QString fault = QString::fromUtf8(argv[5]);

    QString title;
    QString body;
    bool allowCancel = true;
    if (!buildMessage(kind, program, ip, fault, title, body, allowCancel))
    {
        return EXIT_HELPER_FAILURE;
    }

    CrashDialog dialog(title, body, allowCancel);
    const int result = dialog.exec();

    if (result == EXIT_DEBUG || result == EXIT_OK)
    {
        return result;
    }
    return EXIT_HELPER_FAILURE;
}
