#include "microwave.h"
#include "ui_microwave.h"

#include <QDebug>
#include <QUdpSocket>
#include <QNetworkDatagram>
#include <QStateMachine>
#include <QState>
#include <QFinalState>
#include <QTimer>

namespace {
enum class Command : uint32_t {
    NONE            = 0x00000000,
    TIME_COOK       = 0x00000001,
    POWER_LEVEL     = 0x00000002,
    KITCHEN_TIMER   = 0x00000004,
    CLOCK           = 0x00000008,
    DIGIT_0         = 0x00000010,
    DIGIT_1         = 0x00000020,
    DIGIT_2         = 0x00000040,
    DIGIT_3         = 0x00000080,
    DIGIT_4         = 0x00000100,
    DIGIT_5         = 0x00000200,
    DIGIT_6         = 0x00000400,
    DIGIT_7         = 0x00000800,
    DIGIT_8         = 0x00001000,
    DIGIT_9         = 0x00002000,
    STOP            = 0x00004000,
    START           = 0x00008000,
    MOD_LEFT_TENS   = 0x00010000,
    MOD_LEFT_ONES   = 0x00020000,
    MOD_RIGHT_TENS  = 0x00040000,
    MOD_RIGHT_ONES  = 0x00080000,
    CURRENT_POWER   = 0x10000000,
    CURRENT_KITCHEN = 0x20000000,
    CURRENT_COOK    = 0x40000000,
    CURRENT_CLOCK   = 0x80000000
};

const quint32 initialPowerLevel {10};

const quint16 APP_RECV_PORT {64000};
const quint16 SIM_RECV_PORT {64001};
const QHostAddress local {QHostAddress::LocalHost};
QByteArray buf{};
}


Microwave::Microwave(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::Microwave)
    , inSocket{new QUdpSocket()}
    , outSocket{new QUdpSocket()}
    , minuteTimer{new QTimer(this)}
    , secondTimer{new QTimer(this)}
    , clockTime()
    , proposedClockTime()
    , displayTime()
    , powerLevel(initialPowerLevel)
    , setting_clock{false}
    , set_cook_time_initial_digit{true}
    , currentCookIndex{0}
    , currentModState{MOD_STATE::NONE}
    , sm{new QStateMachine(this)}
    , display_clock{Q_NULLPTR}
    , set_cook_time{Q_NULLPTR}
    , set_power_level{Q_NULLPTR}
    , display_timer{Q_NULLPTR}
{
    ui->setupUi(this);
    inSocket->bind(local, SIM_RECV_PORT);

    connect(inSocket, SIGNAL(readyRead()), this, SLOT(readDatagram()));
    connect(minuteTimer, SIGNAL(timeout()), this, SLOT(incrementClockTime()));
    connect(this, SIGNAL(start_sig()), this, SLOT(start_timer()));
    connect(this, SIGNAL(stop_sig()), this, SLOT(stop_timer()));

    minuteTimer->setInterval(60000);
    minuteTimer->setSingleShot(false);

    secondTimer->setInterval(1000);
    secondTimer->setSingleShot(false);

    display_clock = create_display_clock_state(sm);
    set_cook_time = create_set_cook_time_state(sm);
    set_power_level = create_set_power_level_state(sm);
    display_timer = create_display_timer_state(sm);

    display_clock->addTransition(this, SIGNAL(cook_time_sig()), set_cook_time);
    set_cook_time->addTransition(this, SIGNAL(power_level_sig()), set_power_level);
    set_power_level->addTransition(this, SIGNAL(cook_time_sig()), set_cook_time);
    set_cook_time->addTransition(this, SIGNAL(start_sig()), display_timer);
    set_power_level->addTransition(this, SIGNAL(start_sig()), display_timer);
    display_timer->addTransition(this, SIGNAL(display_timer_done_sig()), display_clock);

    sm->setInitialState(display_clock);
    sm->start();
}

Microwave::~Microwave()
{
    delete outSocket;
    delete inSocket;
    delete ui;
}

QState* Microwave::create_display_clock_state(QState *parent)
{
    QState* display_clock {new QState(parent)};
    QState* display_clock_init {new QState(display_clock)};
    QState* set_clock {new QState(display_clock)};
    QState* select_hour_tens {new QState(set_clock)};
    QState* select_hour_ones {new QState(set_clock)};
    QState* select_minute_tens {new QState(set_clock)};
    QState* select_minute_ones {new QState(set_clock)};

    display_clock->setObjectName("display_clock");
    display_clock_init->setObjectName("init");
    set_clock->setObjectName("set_clock");
    select_hour_tens->setObjectName("select_hour_tens");
    select_hour_ones->setObjectName("select_hour_ones");
    select_minute_tens->setObjectName("select_minute_tens");
    select_minute_ones->setObjectName("select_minute_ones");

    //display_clock
    connect(display_clock, SIGNAL(entered()), this, SLOT(display_clock_entry()));
    connect(display_clock, SIGNAL(exited()), this, SLOT(display_clock_exit()));
    display_clock_init->addTransition(this, SIGNAL(clock_sig()), set_clock);

    //set_clock
    connect(set_clock, SIGNAL(entered()), this, SLOT(set_clock_entry()));
    connect(set_clock, SIGNAL(exited()), this, SLOT(set_clock_exit()));
    set_clock->setInitialState(select_hour_tens);

    //select_hour_tens
    connect(select_hour_tens, SIGNAL(entered()), this, SLOT(select_hour_tens_entry()));
    connect(select_hour_tens, SIGNAL(exited()), this, SLOT(select_hour_tens_exit()));
    select_hour_tens->addTransition(this, SIGNAL(next_digit_sig()), select_hour_ones);

    //select_hour_ones
    connect(select_hour_ones, SIGNAL(entered()), this, SLOT(select_hour_ones_entry()));
    connect(select_hour_ones, SIGNAL(exited()), this, SLOT(select_hour_ones_exit()));
    select_hour_ones->addTransition(this, SIGNAL(next_digit_sig()), select_minute_tens);

    //select_minute_tens
    connect(select_minute_tens, SIGNAL(entered()), this, SLOT(select_minute_tens_entry()));
    connect(select_minute_tens, SIGNAL(exited()), this, SLOT(select_minute_tens_exit()));
    select_minute_tens->addTransition(this, SIGNAL(next_digit_sig()), select_minute_ones);

    //select_minute_ones
    connect(select_minute_ones, SIGNAL(entered()), this, SLOT(select_minute_ones_entry()));
    connect(select_minute_ones, SIGNAL(exited()), this, SLOT(select_minute_ones_exit()));
    select_minute_ones->addTransition(this, SIGNAL(next_digit_sig()), set_clock);

    set_clock->addTransition(this, SIGNAL(clock_done_sig()), display_clock);

    display_clock->setInitialState(display_clock_init);
    return display_clock;
}

QState* Microwave::create_set_cook_time_state(QState *parent)
{
    QState* set_cook_time {new QState(parent)};
    QState* cook_timer_initial {new QState(set_cook_time)};

    set_cook_time->setObjectName("set_cook_time");
    cook_timer_initial->setObjectName("cook_timer_initial");

    connect(set_cook_time, SIGNAL(entered()), this, SLOT(set_cook_time_entry()));
    connect(set_cook_time, SIGNAL(exited()), this, SLOT(set_cook_time_exit()));

    set_cook_time->setInitialState(cook_timer_initial);
    return set_cook_time;
}

QState* Microwave::create_set_power_level_state(QState *parent)
{
    QState* set_power_level {new QState(parent)};
    QState* set_power_level_initial {new QState(set_power_level)};

    set_power_level->setObjectName("set_power_level");
    set_power_level_initial->setObjectName("set_power_level_initial");

    connect(set_power_level, SIGNAL(entered()), this, SLOT(set_power_level_entry()));
    connect(set_power_level, SIGNAL(exited()), this, SLOT(set_power_level_exit()));
    set_power_level->setInitialState(set_power_level_initial);
    return set_power_level;
}

QState* Microwave::create_display_timer_state(QState *parent)
{
    QState* display_timer {new QState(parent)};
    QState* display_timer_running {new QState(display_timer)};
    QState* display_timer_paused {new QState(display_timer)};

    connect(display_timer, SIGNAL(entered()), this, SLOT(display_timer_entry()));
    connect(display_timer, SIGNAL(entered()), secondTimer, SLOT(start()));
    connect(display_timer, SIGNAL(exited()), secondTimer, SLOT(stop()));
    connect(display_timer, SIGNAL(exited()), this, SLOT(display_timer_exit()));

    display_timer_running->addTransition(this, SIGNAL(stop_sig()), display_timer_paused);
    display_timer_paused->addTransition(this, SIGNAL(start_sig()), display_timer_running);
    display_timer->setInitialState(display_timer_running);
    return display_timer;
}

void Microwave::display_clock_entry()
{
    ui->textEdit->append("display_clock entered");
}

void Microwave::display_clock_exit()
{
    ui->textEdit->append("display_clock exited");
}

void Microwave::set_clock_entry()
{
    ui->textEdit->append("set_clock entered");
    if(!setting_clock) {
        send_clock_cmd(); //send Command::CLOCK back as an ACK
        connect(this, SIGNAL(clock_sig()), this, SLOT(accept_clock()));
        connect(this, SIGNAL(stop_sig()), this, SLOT(decline_clock()));
        disconnect(this, SIGNAL(start_sig()), this, SLOT(start_timer()));
        disconnect(this, SIGNAL(stop_sig()), this, SLOT(stop_timer()));
        proposedClockTime = clockTime;
        setting_clock = true;
        currentModState = MOD_STATE::SET_CLOCK;
    }
}

void Microwave::set_clock_exit()
{
    ui->textEdit->append("set_clock exited");
    if(!setting_clock) {
        disconnect(this, SIGNAL(clock_sig()), this, SLOT(accept_clock()));
        disconnect(this, SIGNAL(stop_sig()), this, SLOT(decline_clock()));
        connect(this, SIGNAL(start_sig()), this, SLOT(start_timer()));
        connect(this, SIGNAL(stop_sig()), this, SLOT(stop_timer()));
    }
}

void Microwave::select_hour_tens_entry()
{
    ui->textEdit->append("select_hour_tens entered");
    connect(this, SIGNAL(digit_entered(quint32)), this, SLOT(update_left_tens(quint32)));
    //send mod_left_tens cmd
    const Command cmd {Command::MOD_LEFT_TENS};
    QByteArray ba{};
    memcpy(ba.data(), &cmd, sizeof(quint32));
    outSocket->writeDatagram(ba.data(), sizeof(quint32), local, APP_RECV_PORT);
}

void Microwave::select_hour_tens_exit()
{
    ui->textEdit->append("select_hour_tens exited");
    disconnect(this, SIGNAL(digit_entered(quint32)), this, SLOT(update_left_tens(quint32)));
}

void Microwave::select_hour_ones_entry()
{
    ui->textEdit->append("select_hour_ones entered");
    connect(this, SIGNAL(digit_entered(quint32)), this, SLOT(update_left_ones(quint32)));
    //send mod_left_ones cmd
    const Command cmd {Command::MOD_LEFT_ONES};
    QByteArray ba{};
    memcpy(ba.data(), &cmd, sizeof(quint32));
    outSocket->writeDatagram(ba.data(), sizeof(quint32), local, APP_RECV_PORT);
}

void Microwave::select_hour_ones_exit()
{
    ui->textEdit->append("select_hour_ones exited");
    disconnect(this, SIGNAL(digit_entered(quint32)), this, SLOT(update_left_ones(quint32)));
}

void Microwave::select_minute_tens_entry()
{
    ui->textEdit->append("select_minute_tens entered");
    connect(this, SIGNAL(digit_entered(quint32)), this, SLOT(update_right_tens(quint32)));
    //send mod_right_tens cmd
    const Command cmd {Command::MOD_RIGHT_TENS};
    QByteArray ba{};
    memcpy(ba.data(), &cmd, sizeof(quint32));
    outSocket->writeDatagram(ba.data(), sizeof(quint32), local, APP_RECV_PORT);
}

void Microwave::select_minute_tens_exit()
{
    ui->textEdit->append("select_minut_tens exited");
    disconnect(this, SIGNAL(digit_entered(quint32)), this, SLOT(update_right_tens(quint32)));
}

void Microwave::select_minute_ones_entry()
{
    ui->textEdit->append("select_minute_ones entered");
    connect(this, SIGNAL(digit_entered(quint32)), this, SLOT(update_right_ones(quint32)));
    //send mod_right_ones cmd
    const Command cmd {Command::MOD_RIGHT_ONES};
    QByteArray ba{};
    memcpy(ba.data(), &cmd, sizeof(quint32));
    outSocket->writeDatagram(ba.data(), sizeof(quint32), local, APP_RECV_PORT);
}

void Microwave::select_minute_ones_exit()
{
    ui->textEdit->append("select_minute_ones exited");
    disconnect(this, SIGNAL(digit_entered(quint32)), this, SLOT(update_right_ones(quint32)));
}

void Microwave::set_cook_time_entry()
{
    send_cook_time_cmd(); //ack
    set_cook_time_initial_digit = true;
    currentModState = MOD_STATE::SET_COOK;
    connect(this, SIGNAL(digit_entered(quint32)), this, SLOT(update_right_ones(quint32)));
}

void Microwave::set_cook_time_exit()
{
    disconnect(this, SIGNAL(digit_entered(quint32)), this, SLOT(update_right_ones(quint32)));
}

void Microwave::set_power_level_entry()
{
    send_current_power(); //ack
    currentModState = MOD_STATE::SET_POWER;
    connect(this, SIGNAL(digit_entered(quint32)), this, SLOT(update_right_ones(quint32)));
}

void Microwave::set_power_level_exit()
{
    powerLevel = initialPowerLevel;
    disconnect(this, SIGNAL(digit_entered(quint32)), this, SLOT(update_right_ones(quint32)));
}

void Microwave::display_timer_entry()
{
    send_current_timer(); //ack + data
    connect(secondTimer, SIGNAL(timeout()), this, SLOT(decrementTimer()));
}

void Microwave::display_timer_exit()
{
    disconnect(secondTimer, SIGNAL(timeout()), this, SLOT(decrementTimer()));
}

void Microwave::readDatagram()
{
    while(inSocket->hasPendingDatagrams()) {
        QNetworkDatagram datagram {inSocket->receiveDatagram()};
        QByteArray rbuf {datagram.data()};
        Command data {Command::NONE};
        memcpy(&data, rbuf.data(), sizeof(data));

        ui->textEdit->append(("Data = 0x" + QString::number(static_cast<quint32>(data), 16)));
        //now parse data
        switch(data) {
        case Command::TIME_COOK:
            emit cook_time_sig();
            break;
        case Command::POWER_LEVEL:
            emit power_level_sig();
            break;
        case Command::KITCHEN_TIMER:
            //emit set_kitchen_timer();
            break;
        case Command::CLOCK:
            emit clock_sig();
            break;
        case Command::DIGIT_1:
            emit digit_entered(1);
            break;
        case Command::DIGIT_2:
            emit digit_entered(2);
            break;
        case Command::DIGIT_3:
            emit digit_entered(3);
            break;
        case Command::DIGIT_4:
            emit digit_entered(4);
            break;
        case Command::DIGIT_5:
            emit digit_entered(5);
            break;
        case Command::DIGIT_6:
            emit digit_entered(6);
            break;
        case Command::DIGIT_7:
            emit digit_entered(7);
            break;
        case Command::DIGIT_8:
            emit digit_entered(8);
            break;
        case Command::DIGIT_9:
            emit digit_entered(9);
            break;
        case Command::DIGIT_0:
            emit digit_entered(0);
            break;
        case Command::STOP:
            emit stop_sig();
            break;
        case Command::START:
            emit start_sig();
            break;
        case Command::CURRENT_CLOCK:
            send_current_clock();
            break;
        case Command::MOD_LEFT_TENS:
        case Command::MOD_LEFT_ONES:
        case Command::MOD_RIGHT_TENS:
        case Command::MOD_RIGHT_ONES:
        case Command::NONE:
        default:
            //do nothing
            break;
        }
    }
}

void Microwave::incrementClockTime()
{
    //incrementing by 1 minute HH:MM
    ++clockTime.right_ones;
    if(60 == clockTime.right_ones) {
        clockTime.right_ones = 0;
        ++clockTime.right_tens;
        if(60 == clockTime.right_tens) {
            clockTime.right_tens = 0;
            ++clockTime.left_ones;
            if(1 == clockTime.left_tens) {
                if(3 == clockTime.left_ones) {
                    clockTime.left_tens = 0;
                    clockTime.left_ones = 1;
                }
            }
            else {
                if(10 == clockTime.left_ones) {
                    clockTime.left_tens = 1;
                    clockTime.left_ones = 0;
                }
            }
        }
    }

    //send updated clock here
    send_current_clock();
}

void Microwave::decrementTimer()
{
    Time& time {displayTime[currentCookIndex]};
    if(time.right_ones > 0) {
        --time.right_ones;
    }
    else if(0 == time.right_ones) {
        time.right_ones = 9;
        if(time.right_tens > 0) {
            --time.right_tens;
        }
        else if(0 == time.right_tens) {
            time.right_tens = 5;
            if(time.left_ones > 0) {
                --time.left_ones;
            }
            else if(0 == time.left_ones) {
                time.left_ones = 9;
                if(time.left_tens > 0) {
                    --time.left_tens;
                }
            }
        }
    }

    send_current_timer();
    if(0 == time.left_tens && 0 == time.left_ones && 0 == time.right_tens && 0 == time.right_ones) {
        emit display_timer_done_sig();
    }
}

void Microwave::add30Seconds(Time& time)
{
    //construct the seconds and minutes
    quint32 seconds = 10 * time.right_tens + time.right_ones;
    quint32 minutes = 10 * time.left_tens + time.left_ones;

    if(seconds < 30) {
        seconds += 30;
    }
    else {
        const quint32 sectoadd {(60 - seconds) % 60};
        seconds = sectoadd;
        minutes += 1;
    }

    //deconstruct the seconds and minutes back into respective time positions
    time.left_tens = minutes / 10;
    time.left_ones = minutes % 10;
    time.right_tens = seconds / 10;
    time.right_ones = seconds % 10;
}

void Microwave::send_current_clock()
{
    const Command cmd {Command::CURRENT_CLOCK};
    memcpy(buf.data(), &cmd, sizeof(quint32));
    memcpy(buf.data() + sizeof(quint32), &clockTime, sizeof(Time));
    outSocket->writeDatagram(buf.data(), sizeof(quint32) + sizeof(Time), local, APP_RECV_PORT);
}

void Microwave::send_current_cook()
{
    const Command cmd {Command::CURRENT_COOK};
    memcpy(buf.data(), &cmd, sizeof(quint32));
    memcpy(buf.data() + sizeof(quint32), &displayTime, sizeof(Time));
    outSocket->writeDatagram(buf.data(), sizeof(quint32) + sizeof(Time), local, APP_RECV_PORT);
}

void Microwave::send_current_power()
{
    const Command cmd {Command::CURRENT_POWER};
    memcpy(buf.data(), &cmd, sizeof(quint32));
    memcpy(buf.data() + sizeof(quint32), &powerLevel, sizeof(quint32));
    outSocket->writeDatagram(buf.data(), 2 * sizeof(quint32), local, APP_RECV_PORT);
}

void Microwave::send_current_timer()
{
    const Command cmd {Command::CURRENT_COOK};
    memcpy(buf.data(), &cmd, sizeof(quint32));
    memcpy(buf.data() + sizeof(quint32), &displayTime, sizeof(Time));
    outSocket->writeDatagram(buf.data(), sizeof(quint32) + sizeof(Time), local, APP_RECV_PORT);
}

void Microwave::send_cook_time_cmd()
{
    const Command cmd {Command::TIME_COOK};
    memcpy(buf.data(), &cmd, sizeof(quint32));
    outSocket->writeDatagram(buf.data(), sizeof(quint32), local, APP_RECV_PORT);
}

void Microwave::send_clock_cmd()
{
    const Command cmd {Command::CLOCK};
    memcpy(buf.data(), &cmd, sizeof(quint32));
    outSocket->writeDatagram(buf.data(), sizeof(quint32), local, APP_RECV_PORT);
}

void Microwave::send_stop_cmd()
{
    const Command cmd {Command::STOP};
    memcpy(buf.data(), &cmd, sizeof(quint32));
    outSocket->writeDatagram(buf.data(), sizeof(quint32), local, APP_RECV_PORT);
}

void Microwave::send_digit_cmd(const quint32 digit)
{
    const Command cmd {static_cast<Command>((1 << 4) << digit)};
    memcpy(buf.data(), &cmd, sizeof(quint32));
    outSocket->writeDatagram(buf.data(), sizeof(quint32), local, APP_RECV_PORT);
}

void Microwave::accept_clock()
{
    setting_clock = false;
    clockTime = proposedClockTime;
    // start the minute timer if it hasn't been started
    if(!minuteTimer->isActive()) {
        minuteTimer->start();
    }
    send_clock_cmd();
    emit clock_done_sig();
}

void Microwave::decline_clock()
{
    setting_clock = false;
    send_stop_cmd();
    emit clock_done_sig();
}

void Microwave::start_timer()
{
    switch(currentModState) {
    case MOD_STATE::SET_COOK:
    case MOD_STATE::SET_POWER:
        currentModState = MOD_STATE::NONE;
        emit start_display_timer_sig();
        break;
    case MOD_STATE::SET_KITCHEN:
        currentModState = MOD_STATE::NONE;
        emit start_display_timer_sig();
        break;
    case MOD_STATE::NONE:
        add30Seconds(displayTime[currentCookIndex]);
        send_current_timer();
        break;
    case MOD_STATE::SET_CLOCK:
    default:
        break;
    }
}

void Microwave::stop_timer()
{
    send_stop_cmd();
}

void Microwave::update_left_tens(quint32 digit)
{
    switch(currentModState) {
    case MOD_STATE::SET_CLOCK:
        if(digit > 1) return;
        proposedClockTime.left_tens = digit;
        //convert number to Command and send out via udp socket
        send_digit_cmd(digit);
        break;
    default:
        break;
    }

    emit next_digit_sig();
}

void Microwave::update_left_ones(quint32 digit)
{
    switch(currentModState) {
    case MOD_STATE::SET_CLOCK:
        if(0 == proposedClockTime.left_tens && digit > 0) {
            proposedClockTime.left_ones = digit;
        }
        else if(1 == proposedClockTime.left_tens && digit < 3) {
            proposedClockTime.left_ones = digit;
        }
        else {
            return;
        }
        //convert number to Command and send out via udp socket
        send_digit_cmd(digit);
        break;
    default:
        break;
    }

    emit next_digit_sig();
}

void Microwave::update_right_tens(quint32 digit)
{
    switch(currentModState) {
    case MOD_STATE::SET_CLOCK:
        if(digit > 5) return;
        proposedClockTime.right_tens = digit;
        //convert number to Command and send out via udp socket
        send_digit_cmd(digit);
        break;
    default:
        break;
    }

    emit next_digit_sig();
}

void Microwave::update_right_ones(quint32 digit)
{
    switch(currentModState) {
    case MOD_STATE::SET_CLOCK: {
            proposedClockTime.right_ones = digit;
            //convert number to Command and send out via udp socket
            send_digit_cmd(digit);
        }
        break;
    case MOD_STATE::SET_COOK: {
            Time& time {displayTime[currentCookIndex]};
            if(set_cook_time_initial_digit && digit > 0) {
                time.right_ones = digit;
                set_cook_time_initial_digit = false;
            }
            else {
                time.left_tens = time.left_ones;
                time.left_ones = time.right_tens;
                time.right_tens = time.right_ones;
                time.right_ones = digit;
            }
            send_current_timer();
        }
        break;
    case MOD_STATE::SET_POWER:
        if(1 == powerLevel) {
            if(0 == digit) {
                powerLevel = 10;
            }
            else {
                powerLevel = digit;
            }
        }
        powerLevel = digit;
        send_current_power();
        return;
    default:
        break;
    }

    emit next_digit_sig();
}