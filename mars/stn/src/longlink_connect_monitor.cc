// Tencent is pleased to support the open source community by making Mars available.
// Copyright (C) 2016 THL A29 Limited, a Tencent company. All rights reserved.

// Licensed under the MIT License (the "License"); you may not use this file except in 
// compliance with the License. You may obtain a copy of the License at
// http://opensource.org/licenses/MIT

// Unless required by applicable law or agreed to in writing, software distributed under the License is
// distributed on an "AS IS" basis, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
// either express or implied. See the License for the specific language governing permissions and
// limitations under the License.


/*
 * longlink_connect_monitor.cc
 *
 *  Created on: 2014-2-26
 *      Author: yerungui
 */

#include "longlink_connect_monitor.h"

#include "boost/bind.hpp"

#include "mars/app/app.h"
#include "mars/baseevent/active_logic.h"
#include "mars/comm/thread/lock.h"
#include "mars/comm/xlogger/xlogger.h"
#include "mars/comm/time_utils.h"
#include "mars/comm/socket/unix_socket.h"
#include "mars/comm/platform_comm.h"
#include "mars/sdt/src/checkimpl/dnsquery.h"
#include "mars/stn/config.h"

#include "longlink_speed_test.h"
#include "net_source.h"

#ifdef __ANDROID__
#include <sstream>
#include <android/log.h>
#endif

using namespace mars::stn;
using namespace mars::app;

static const unsigned int kTimeCheckPeriod = 10 * 1000;     // 10s
static const unsigned int kStartCheckPeriod = 3 * 1000;     // 3s
static const unsigned int kInactiveBuffer = 30 * 1000;      //30s

static const unsigned long kNoNetSaltRate = 3;
static const unsigned long kNoNetSaltRise = 600;
static const unsigned long kNoAccountInfoSaltRate = 2;
static const unsigned long kNoAccountInfoSaltRise = 300;

static const unsigned long kNoAccountInfoInactiveInterval = (7 * 24 * 60 * 60);  // s


#ifdef __ANDROID__
static const int kAlarmType = 102;
#endif

enum {
    kTaskConnect,
    kLongLinkConnect,
    kNetworkChangeConnect,
};

enum {
    kForgroundOneMinute,
    kForgroundTenMinute,
    kForgroundActive,
    kBackgroundActive,
    kInactive,
};

static unsigned long const sg_interval[][5]  = {
    {5,  10, 20,  30,  300},
    {15, 30, 240, 300, 600},
    {0,  0,  0,   0,   0},
};

static std::string alarm_reason;
static std::string error_msg;

static void __AddLogToSystemLogger(const std::stringstream& _logger) {
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "longlink_progress", "%s", _logger.str().data());
#endif
}

static void __LongLinkStateToSystemLog(LongLink::TLongLinkStatus _status) {
#ifdef __ANDROID__

    xinfo_function();
    std::stringstream logger;
    logger << "longlink state: ";
    uint64_t current_time = ::gettickcount();

    switch (_status) {
        case LongLink::kConnecting:
            logger << "connecting time: ";
            break;
        case LongLink::kConnected:
            logger << "connected time: ";
            break;
        case LongLink::kDisConnected:
            logger << "disconnect reason " << error_msg << ", disconnected time: ";
            break;
        case LongLink::kConnectFailed:

            logger << "connect failed " << error_msg << ", disconnected time: ";
            break;
        default:
            return;
    }
    if (LongLink::kConnectFailed == _status || LongLink::kDisConnected == _status) {
        alarm_reason = "prepare to reconnect alarm";
    }   
    logger << current_time;
    __android_log_print(ANDROID_LOG_INFO, "longlink_progress", "%s", logger.str().data());

#endif
}


static int __CurActiveState(const ActiveLogic& _activeLogic) {
    if (!_activeLogic.IsActive()) return kInactive;

    if (!_activeLogic.IsForeground()) return kBackgroundActive;

    if (10 * 60 * 1000 <= ::gettickcount() - _activeLogic.LastForegroundChangeTime()) return kForgroundActive;

    if (60 * 1000 <= ::gettickcount() - _activeLogic.LastForegroundChangeTime()) return kForgroundTenMinute;

    return kForgroundOneMinute;
}

static unsigned long __Interval(int _type, const ActiveLogic& _activelogic) {
    unsigned long interval = sg_interval[_type][__CurActiveState(_activelogic)];

    if (kLongLinkConnect != _type) return interval;

    if (__CurActiveState(_activelogic) == kInactive || __CurActiveState(_activelogic) == kForgroundActive) {  // now - LastForegroundChangeTime>10min
        if (!_activelogic.IsActive() && GetAccountInfo().username.empty()) {
            interval = kNoAccountInfoInactiveInterval;
            xwarn2(TSF"no account info and inactive, interval:%_", interval);

        } else if (kNoNet == getNetInfo()) {
            interval = interval * kNoNetSaltRate + kNoNetSaltRise;
            xinfo2(TSF"no net, interval:%0", interval);

        } else if (GetAccountInfo().username.empty()) {
            interval = interval * kNoAccountInfoSaltRate + kNoAccountInfoSaltRise;
            xinfo2(TSF"no account info, interval:%0", interval);

        } else {
            // default value
			interval += rand() % (20);
        }
    }

    return interval;
}

#define AYNC_HANDLER asyncreg_.Get()

LongLinkConnectMonitor::LongLinkConnectMonitor(ActiveLogic& _activelogic, LongLink& _longlink, MessageQueue::MessageQueue_t _id)
    : asyncreg_(MessageQueue::InstallAsyncHandler(_id))
    , activelogic_(_activelogic), longlink_(_longlink), alarm_(boost::bind(&LongLinkConnectMonitor::__OnAlarm, this), _id)
    , status_(LongLink::kDisConnected)
    , last_connect_time_(0)
    , last_connect_net_type_(kNoNet)
    , thread_(boost::bind(&LongLinkConnectMonitor::__Run, this), XLOGGER_TAG"::con_mon")
    , conti_suc_count_(0)
    , isstart_(false) {
    xinfo2(TSF"handler:(%_,%_)", asyncreg_.Get().queue,asyncreg_.Get().seq);
    activelogic_.SignalActive.connect(boost::bind(&LongLinkConnectMonitor::__OnSignalActive, this, _1));
    activelogic_.SignalForeground.connect(boost::bind(&LongLinkConnectMonitor::__OnSignalForeground, this, _1));
    longlink_.SignalConnection.connect(boost::bind(&LongLinkConnectMonitor::__OnLongLinkStatuChanged, this, _1));
#ifdef __ANDROID__
    alarm_.SetType(kAlarmType);
#endif
}

LongLinkConnectMonitor::~LongLinkConnectMonitor() {
#ifdef __APPLE__
    __StopTimer();
#endif
    longlink_.SignalConnection.disconnect(boost::bind(&LongLinkConnectMonitor::__OnLongLinkStatuChanged, this, _1));
    activelogic_.SignalForeground.disconnect(boost::bind(&LongLinkConnectMonitor::__OnSignalForeground, this, _1));
    activelogic_.SignalActive.disconnect(boost::bind(&LongLinkConnectMonitor::__OnSignalActive, this, _1));
    asyncreg_.CancelAndWait();
}

bool LongLinkConnectMonitor::MakeSureConnected() {
    __IntervalConnect(kTaskConnect);
    return LongLink::kConnected == longlink_.ConnectStatus();
}

bool LongLinkConnectMonitor::NetworkChange() {
    xinfo_function();
#ifdef __APPLE__
    __StopTimer();

    do {
        if (LongLink::kConnected != status_ || (::gettickcount() - last_connect_time_) <= 10 * 1000) break;

        if (kMobile != last_connect_net_type_) break;

        int netifo = getNetInfo();

        if (kNoNet == netifo) break;

        if (__StartTimer()) return false;
    } while (false);

#endif
    longlink_.Disconnect(LongLink::kNetworkChange);

#ifdef __ANDROID__
    std::stringstream logger;
    logger << "network change time: " << ::gettickcount();
    __AddLogToSystemLogger(logger);
#endif

    return 0 == __IntervalConnect(kNetworkChangeConnect);
}

uint64_t LongLinkConnectMonitor::__IntervalConnect(int _type) {
    if (LongLink::kConnecting == longlink_.ConnectStatus() || LongLink::kConnected == longlink_.ConnectStatus()) return 0;

    uint64_t interval =  __Interval(_type, activelogic_) * 1000ULL;
    uint64_t posttime = gettickcount() - longlink_.Profile().dns_time;
    uint64_t buffer = activelogic_.IsActive() ? 0 : kInactiveBuffer;    //in case doze mode

#ifdef __ANDROID__
    std::stringstream logger;
    logger << "next connect interval: " << interval << ", "
            << "process active state: " << activelogic_.IsActive() << ", "
            << "process foreground: " << activelogic_.IsForeground();
    __AddLogToSystemLogger(logger);
#endif

    xinfo2(TSF"next connect interval: %_, posttime: %_, buffer: %_, _type: %_", interval, posttime, buffer, _type);

    
    if ((posttime + buffer) >= interval) {
        bool newone = false;
        bool ret = longlink_.MakeSureConnected(&newone);
        xinfo2(TSF"made interval connect interval:%0, posttime:%_, newone:%_, connectstatus:%_, ret:%_", interval, posttime, newone, longlink_.ConnectStatus(), ret);
        return 0;

    } else {
        return interval - posttime;
    }
}

uint64_t LongLinkConnectMonitor::__AutoIntervalConnect() {
    alarm_.Cancel();
    uint64_t remain = __IntervalConnect(kLongLinkConnect);

    if (0 == remain) return remain;

    xinfo2(TSF"start auto connect after:%0", remain);
    alarm_.Start((int)remain);

#ifdef __ANDROID__
    std::stringstream logger;
    logger  << "set rebuild alarm, reason: rebuild longlink alarm, "
            << "current time: " << ::gettickcount()
            <<  "next rebuild time interval: " << remain;
    alarm_reason = "rebuild alarm";
    __AddLogToSystemLogger(logger);
#endif

    return remain;
}

void LongLinkConnectMonitor::__OnSignalForeground(bool _isForeground) {
    ASYNC_BLOCK_START
#ifdef __APPLE__
    xinfo2(TSF"forground:%_ time:%_ tick:%_", _isForeground, timeMs(), gettickcount());

    if (_isForeground) {
        xinfo2(TSF"longlink:%_ time:%_ %_ %_", longlink_.ConnectStatus(), tickcount_t().gettickcount().get(), longlink_.GetLastRecvTime().get(), int64_t(tickcount_t().gettickcount() - longlink_.GetLastRecvTime()));
        
        if ((longlink_.ConnectStatus() == LongLink::kConnected) &&
                (tickcount_t().gettickcount() - longlink_.GetLastRecvTime() > tickcountdiff_t(4.5 * 60 * 1000))) {
            xwarn2(TSF"sock long time no send data, close it");
            __ReConnect();
        }
    }

#endif
    __AutoIntervalConnect();
    ASYNC_BLOCK_END
}

void LongLinkConnectMonitor::__OnSignalActive(bool _isactive) {
    ASYNC_BLOCK_START
    __AutoIntervalConnect();
    ASYNC_BLOCK_END
}

void LongLinkConnectMonitor::__OnLongLinkStatuChanged(LongLink::TLongLinkStatus _status) {
    xinfo2(TSF"longlink status change: %_ ", _status);
    alarm_.Cancel();

    if (LongLink::kConnectFailed == _status || LongLink::kDisConnected == _status) {
        alarm_.Start(500);
    } else if (LongLink::kConnected == _status) {
        xinfo2(TSF"cancel auto connect");
    }

    status_ = _status;
    last_connect_time_ = ::gettickcount();
    last_connect_net_type_ = ::getNetInfo();


    if (LongLink::kDisConnected == _status || LongLink::kConnectFailed == _status) {
        error_msg = longlink_.GetDisconnectReasonText();
    }

    __LongLinkStateToSystemLog(_status);
}

void LongLinkConnectMonitor::__OnAlarm() {
    __AutoIntervalConnect();
#ifdef __ANDROID__
    std::stringstream logger;
    logger << "onalarm log, reason: " << alarm_reason << ", alarm after " << alarm_.After() << " millsecond, spend time " << alarm_.ElapseTime();
    __AddLogToSystemLogger(logger);
#endif
}

#ifdef __APPLE__
bool LongLinkConnectMonitor::__StartTimer() {
    xdebug_function();

    conti_suc_count_ = 0;

    ScopedLock lock(testmutex_);
    isstart_ = true;

    if (thread_.isruning()) {
        return true;
    }

    int ret = thread_.start_periodic(kStartCheckPeriod, kTimeCheckPeriod);
    return 0 == ret;
}


bool LongLinkConnectMonitor::__StopTimer() {
    xdebug_function();

    ScopedLock lock(testmutex_);

    if (!isstart_) return true;

    isstart_ = false;

    if (!thread_.isruning()) {
        return true;
    }

    thread_.cancel_periodic();


    thread_.join();
    return true;
}
#endif


void LongLinkConnectMonitor::__Run() {
    int netifo = getNetInfo();

    if (LongLink::kConnected != status_ || (::gettickcount() - last_connect_time_) <= 12 * 1000
            || kMobile != last_connect_net_type_ || kMobile == netifo) {
        thread_.cancel_periodic();
        return;
    }

    struct socket_ipinfo_t dummyIpInfo;
    int ret = socket_gethostbyname(NetSource::GetLongLinkHosts().front().c_str(), &dummyIpInfo, 0, NULL);

    if (ret == 0) {
        ++conti_suc_count_;
    } else {
        conti_suc_count_ = 0;
    }

    if (conti_suc_count_ >= 3) {
        __ReConnect();
        thread_.cancel_periodic();
    }
}

void LongLinkConnectMonitor::__ReConnect() {
    xinfo_function();
    xassert2(fun_longlink_reset_);
    fun_longlink_reset_();
}


void LongLinkConnectMonitor::OnHeartbeatAlarmSet(uint64_t _interval) {
#ifdef __ANDROID__
    std::stringstream logger;
    logger << "on_heartbeat_set, time: " << ::gettickcount() << ", "
           << "time interval:" << _interval;

    __AddLogToSystemLogger(logger);
#endif
}

void LongLinkConnectMonitor::OnHeartbeatAlarmReceived(bool _is_noop_timeout) {
#ifdef __ANDROID__
    std::stringstream logger;
    logger << "on_heartbeat_alarm, time: " << ::gettickcount() << ", "
           << "is_noop_timeout:" << _is_noop_timeout;
    __AddLogToSystemLogger(logger);
#endif
}


