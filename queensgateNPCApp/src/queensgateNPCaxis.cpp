#include <sstream>
#include <stdlib.h>
#include <string.h>

#include <epicsExport.h>
#include <epicsTime.h>
#include <epicsThread.h>
#include <epicsEvent.h>
#include <epicsMutex.h>
#include <iocsh.h>
#include <asynOctetSyncIO.h>

#include "queensgateNPCaxis.hpp"

#define QGATE_NUM_PARAMS 100

/*
 * Driver object for stage (axis) control
 */
QgateAxis::QgateAxis(QgateController &controller,
                    unsigned int axisNumber,
                    const char *axisName,
                    unsigned char axisType) :
        asynMotorAxis(&controller, axisNumber-1) //axis Num [1..n] converted to Index [0..n]
        , ctrler(controller)
        , qg(controller.getAdapter())
        , axisNum(axisNumber)
        , axis_name(axisName)
        , isSensor(axisType == AXISTYPE_SENSOR)
        , initialStatus(false)
        , connected(false)
        , _pollCounter(SLOW_POLL_FREQ_CONST)
{
    asynPrint(pasynUser_, ASYN_TRACE_FLOW, "creating QgateAxis %d '%s' %d\n", axisNumber, axisName, axisType);

    //XXX:    
    //setIntegerParam(ctrler.motorStatus_, 0);
    //setIntegerParam(ctrler.motorStatusCommsError_, 1);
    //setIntegerParam(ctrler.motorPowerStatus_, 0);
    setIntegerParam(ctrler.motorPowerOnDelay_, 0);
    setIntegerParam(ctrler.motorPowerOffDelay_, 0);
    setIntegerParam(ctrler.motorPowerAutoOnOff_, 0);

}

QgateAxis::~QgateAxis() {}

bool QgateAxis::initAxis() {
    std::string value;
    bool result = false;
    
    //EPICS is not in charge of the closed loop operation, it is the Queensgate controller,
    // but nevertheless it is defined a closed loop.
    setClosedLoop(true);
    setIntegerParam(ctrler.motorStatusFollowingError_, 0);
    ctrler.deferredMove[axisNo_].clear();
    result = getStatusConnected();
    if(result) {
        ctrler.getCmd("identity.stage.part.get", axisNum, value);
        setStringParam(ctrler.QG_AxisModel, value.c_str());
        initialStatus = true;   //Specify initial moving query for first poll
    }
    return result;
}

/** Poll the axis, start moves when possible, etc.  This function is
 * entered with the lock already on.
 * \param[out] moving Set to TRUE if the axis is moving
 */
asynStatus QgateAxis::poll(bool *moving) {
    bool result = false;
    bool wasconnected = connected;

    //Update initial status
    if(initialStatus) {
        initialStatus = false;
        *moving = false;
        // setIntegerParam(ctrler.motorPowerOnDelay_, 0);
        // setIntegerParam(ctrler.motorPowerOffDelay_, 0);
        // setIntegerParam(ctrler.motorPowerAutoOnOff_, 0);
        return asynSuccess;   //First run of the poll
    }

    asynPrint(ctrler.pasynUserSelf, ASYN_TRACEIO_DRIVER, "Axis %d (%d) %s polling: %sconnected \tSTATUS=%d\n", 
                    axisNum, axisNo_, axis_name.c_str(), (connected)? "":"DIS", status_.status);

    if(ctrler.connected) {
        //fast poll
        result |= getStatusConnected();
        if(result) {
            result |= getPosition();    
            result |= getStatusMoving(*moving);
        }
        //slow poll
        if (_pollCounter++ % SLOW_POLL_FREQ_CONST == 0) {
            result |= getAxisMode();
        }
    } else {
        //Axis reconnection poll (slow)
        *moving = false;
        //Slow poll
        if (_pollCounter++ % SLOW_POLL_FREQ_CONST == 0) {
            result |= getStatusConnected();     //Update Axis status
        }
    }
    //TODO: check !result and if != connected then failed after getStatusConnected()call
    //TODO: check !result and log it
    //Detect connection state change again for additional actions
    if(wasconnected != connected) {
        if(wasconnected) {
            //Just lost connection
            *moving = false;    
            forceStop = false;
        } else {
            initAxis();         //Been re-connected
        }
    }
    callParamCallbacks();
    return asynSuccess;   
}

bool QgateAxis::getStatusConnected() {
    bool result = false;    //Default for when controller not connected

    if(ctrler.connected) {
        std::string value;
        if(ctrler.getCmd("stage.status.stage-connected.get", axisNum, value) == DLL_ADAPTER_STATUS_SUCCESS) {
            result = atoi(value.c_str());
        }
    }
    if(result != connected) {
        //Update axis status on change
        connected = result;
        //Update status bits
        ctrler.setIntegerParam(axisNo_, ctrler.motorStatusProblem_, !connected);
        ctrler.setIntegerParam(axisNo_, ctrler.motorStatusCommsError_, !connected);
        setIntegerParam(ctrler.motorStatus_, !connected);
        setIntegerParam(ctrler.motorStatusProblem_, !connected);
        setIntegerParam(ctrler.motorStatusCommsError_, !connected);
        //Update status of related PVs
        setIntegerParam(ctrler.QG_AxisConnected, connected);
    }
    return connected;
}

bool QgateAxis::getAxisMode() {
    std::string value;
    if(ctrler.getCmd("stage.mode.digital-command.get", axisNum, value) == DLL_ADAPTER_STATUS_SUCCESS) {
        setIntegerParam(ctrler.QG_AxisMode, atoi(value.c_str()));
        return true;
    }
    return false;
}

/** Confirm the stage reached a low-pass filter confirmed position, and update status.
  * Returns false when comms or command failed  */
bool QgateAxis::getInPositionLPF() {
    bool result = false;   //Assume feedback from controller failed
    std::string value;
    if(ctrler.getCmd("stage.status.in-position.lpf-confirmed.get", axisNum, value) == DLL_ADAPTER_STATUS_SUCCESS) {
        int inPos = atoi(value.c_str());
        result = true;      //success getting position status
        forceStop = false;  //Already in position: no need to force stop
        setIntegerParam(ctrler.QG_AxisInPosLPF, inPos);
    }
    return result;   //failed/success
}

/** Confirm the stage reached position, and update status.
  * Returns false when comms or command failed  */
bool QgateAxis::getInPositionUnconfirmed() {
    bool result = false;   //Assume feedback from controller failed
    std::string value;
    if(ctrler.getCmd("stage.status.in-position.unconfirmed.get", axisNum, value) == DLL_ADAPTER_STATUS_SUCCESS) {
        int inPos = atoi(value.c_str());
        result = true;      //success getting position status
        setIntegerParam(ctrler.QG_AxisInPosUnconfirmed, inPos);
    }
    return result;   //failed/success
}

/** Confirm the stage reached a window filter confirmed position, and update status.
  * Returns false when comms or command failed  */
bool QgateAxis::getInPositionWindow() {
    bool result = false;   //Assume feedback from controller failed
    std::string value;
    if(ctrler.getCmd("stage.status.in-position.window-filter-confirmed.get", axisNum, value) == DLL_ADAPTER_STATUS_SUCCESS) {
        int inPos = atoi(value.c_str());
        result = true;      //success getting position status
        setIntegerParam(ctrler.QG_AxisInPosWindow, inPos);
    }
    return result;   //failed/success
}

/** Gets the moving status, and confirms position reached if not moving.
  * Returns false when comms or command failed  */
bool QgateAxis::getStatusMoving(bool &moving) {
    bool result = false;    //Assume feedback from controller failed

    if(isSensor) {
        moving = false;     //Sensor never have indication of being moved
        result = true;      //Assume success comms
    } else {
        std::string value;
        if(ctrler.getCmd("stage.status.stage-moving.get", axisNum, value) == DLL_ADAPTER_STATUS_SUCCESS) {
            int isMoving = atoi(value.c_str());
            moving = isMoving;  //int to bool conversion
            if(!moving) {
                result = getInPositionUnconfirmed();   
                result |= getInPositionLPF();
                result |= getInPositionWindow();
            }
        }
    }
    //Update Motor Record status
    setIntegerParam(ctrler.motorStatusDone_, !moving);
    setIntegerParam(ctrler.motorStatusMoving_, moving);

    return result;
}

bool QgateAxis::getPosition() {
    std::string value;
    if(ctrler.getCmd("stage.position.measured.get", axisNum, value) != DLL_ADAPTER_STATUS_SUCCESS) {
        return false;
    }
    //Report position
    double positionMicrons = PM_TO_MICRONS(atof(value.c_str()));
    double position = atof(value.c_str());
    asynPrint(pasynUser_, ASYN_TRACEIO_DEVICE, "Queensgate %s Axis %d measured pos=%lf microns (%lf pm)\n", 
                ctrler.nameCtrl.c_str(), axisNum, positionMicrons, position);

    setDoubleParam(ctrler.motorEncoderPosition_, position);
    setDoubleParam(ctrler.motorPosition_, position);
    return true;
}

/** Move the motor to an absolute location or by a relative amount.
  * \param[in] position  The absolute position to move to (if relative=0) or the relative distance to move 
  * by (if relative=1). Units=microns.
  * \param[in] relative  Flag indicating relative move (1) or absolute move (0). [NOT YET IMPLEMENTED in this method -- only absolute move]
  * \param[in] minVelocity The initial velocity, often called the base velocity. Units=steps/sec. [IGNORED in this method]
  * \param[in] maxVelocity The maximum velocity, often called the slew velocity. Units=steps/sec. [IGNORED in this method]
  * \param[in] acceleration The acceleration value. Units=steps/sec/sec. [IGNORED in this method]  */
asynStatus QgateAxis::move(double position, int relative,
            double minVelocity, double maxVelocity, double acceleration) {
    
    asynPrint(pasynUser_, ASYN_TRACE_FLOW, "::::MOVE axis axis %s-%d request: pos=%lf, relative=%d, minV=%lf, maxV=%lf, acc=%lf\n",
                ctrler.nameCtrl.c_str(), axisNum, position, relative, minVelocity, maxVelocity, acceleration);
    if(!connected) {
        // If axis not connected to a stage, ignore
        return asynSuccess;
    }
    if(isSensor) {
        asynPrint(ctrler.pasynUserSelf, ASYN_TRACEIO_DEVICE, ":::::MOVE axis %s-%d denied: is a sensor\n", ctrler.nameCtrl.c_str(), axisNum);
        return asynError;   //Refuse moving on Sensors
    }

    // Start the move
	TakeLock takeLock(&ctrler, /*alreadyTaken=*/true);
    FreeLock freeLock(takeLock);

    //Note: NPC controller have pre-configured movement parameters (e.g. velocity, accel)
    if(ctrler.moveCmd("stage.position.absolute-command.set", axisNum, position) != DLL_ADAPTER_STATUS_SUCCESS) {
        //TODO: move this to centralized status?
        connected = false;
        ctrler.setIntegerParam(axisNo_, ctrler.motorStatusCommsError_, !connected);
        
        return asynError;
    } else {
        forceStop = false;  //Cancel any previous stop request
    }

    if(relative) {
        //TODO: relative move
    //     ctrler.moveCmd("stage.position.command.set", physicalAxis(), position);
    //     //ctrler.doMove(physicalAxis(), position);
    // } else {
    //     ctrler.moveCmd("stage.position.absolute-command.set", physicalAxis(), position);
    // }
    // setIntegerParam(ctrler.motorStatusDone_, 0);
    // setIntegerParam(ctrler.motorStatusDirection_, (int)position > curPosition);

    // Wait for move to complete. 
    // std::string sstat; 
    // asynStatus status = asynError;
    // int curStatus;
    // bool moving = true;
    // bool inPos;
    // do {
    //     TakeLock again(freeLock);
    //     inPos = getStatusMoving();
    // //     result = ctrler.getCmd("stage.status.in-position.lpf-confirmed.get", 
    // //                             physicalAxis(), sstat);
    // //     if(result != DLL_ADAPTER_STATUS_SUCCESS)
    // //     {
    // //         printf("QgateNPC: Error checking moving status\n");
    // //         moving = false;
    // //     } else {
    // //         curStatus = atoi(sstat.c_str());
    // //         moving = !(curStatus == 3 || curStatus == 0);
    // //         if (!moving) {
    // //             status = asynSuccess;

    // //         }
    // //         //TODO: delay here to avoid hammering
    // //         //TODO: timeout of full move
    // //     }
    // } while(!inPos);

    // // Do a poll now
    // // pollStatus(freeLock);
    // return status;
    }
    return asynSuccess;
}

asynStatus QgateAxis::stop(double acceleration) {
    asynStatus status = asynError;
    if(!connected) {
        // If axis not connected to a stage, ignore
        return asynSuccess;
    }
    if(isSensor) {
        return asynError;   //Refuse moving-related commands on Sensors
    }
    //clear axis' pending deferred command
    if(ctrler.deferringMode) {
        ctrler.deferredMove[axisNo_].clear();
        return asynSuccess;
    }

    //Re-command the axis to be in the current position
    // Start the stop procedure
	TakeLock takeLock(&ctrler, /*alreadyTaken=*/true);
    FreeLock freeLock(takeLock);
    forceStop = true;
    double newPosition;
    if(!getPosition()) {
        asynPrint(pasynUser_, ASYN_TRACEIO_DEVICE, ":::::STOP axis %s-%d ignoring stop command\n", ctrler.nameCtrl.c_str(), axisNum);
        return asynSuccess;
    }
    ctrler.getDoubleParam(axisNo_, ctrler.motorPosition_, &newPosition);
    //Note: NPC controller have pre-configured movement parameters (e.g. velocity, accel)
    if(ctrler.moveCmd("stage.position.absolute-command.set", axisNum, newPosition) != DLL_ADAPTER_STATUS_SUCCESS) {
        status = asynError;
    }       
    else {
        asynPrint(pasynUser_, ASYN_TRACEIO_FILTER, ":::::STOP axis %s-%d at raw pos %lf\n", ctrler.nameCtrl.c_str(), axisNum, newPosition);
        status = asynSuccess;
    }

    return status;
}
