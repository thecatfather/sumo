/****************************************************************************/
// Eclipse SUMO, Simulation of Urban MObility; see https://eclipse.org/sumo
// Copyright (C) 2001-2018 German Aerospace Center (DLR) and others.
// This program and the accompanying materials
// are made available under the terms of the Eclipse Public License v2.0
// which accompanies this distribution, and is available at
// http://www.eclipse.org/legal/epl-v20.html
// SPDX-License-Identifier: EPL-2.0
/****************************************************************************/
/// @file    MSCFModel_CACC.cpp
/// @author  Kallirroi Porfyri
/// @date    Feb 2018
/// @version $Id$
///
// ACC car-following model based on [1], [2].
// [1] Milanes, V., and S. E. Shladover. Handling Cut-In Vehicles in Strings
//    of Cooperative Adaptive Cruise Control Vehicles. Journal of Intelligent
//     Transportation Systems, Vol. 20, No. 2, 2015, pp. 178-191.
// [2] Xiao, L., M. Wang and B. van Arem. Realistic Car-Following Models for
//    Microscopic Simulation of Adaptive and Cooperative Adaptive Cruise
//     Control Vehicles. Transportation Research Record: Journal of the
//     Transportation Research Board, No. 2623, 2017. (DOI: 10.3141/2623-01).
/****************************************************************************/


// ===========================================================================
// included modules
// ===========================================================================
#include <config.h>

#include <stdio.h>
#include <iostream>

#include "MSCFModel_ACC.h"
#include <microsim/MSVehicle.h>
#include <microsim/MSLane.h>
#include <utils/common/RandHelper.h>
#include <utils/common/SUMOTime.h>
#include <microsim/lcmodels/MSAbstractLaneChangeModel.h>
#include <math.h>
#include <microsim/MSNet.h>


// ===========================================================================
// method definitions
// ===========================================================================
MSCFModel_ACC::MSCFModel_ACC(const MSVehicleType* vtype, double maxAccel, double maxDecel,
   double emergencyDecel, double headwayTime, double SpeedControlGain, double GapClosingControlGainSpeed, double GapClosingControlGainSpace, double GapControlGainSpeed, double GapControlGainSpace)
   : MSCFModel(vtype, maxAccel, maxDecel, emergencyDecel, maxDecel, headwayTime)
{
   myAccel = maxAccel;
   myDecel = maxDecel;
   myheadwayTime = headwayTime;
   mySpeedControlGain = SpeedControlGain;
   myGapClosingControlGainSpeed = GapClosingControlGainSpeed;
   myGapClosingControlGainSpace = GapClosingControlGainSpace;
   myGapControlGainSpeed = GapControlGainSpeed;
   myGapControlGainSpace = GapControlGainSpace;
}

MSCFModel_ACC::~MSCFModel_ACC() {}


double
MSCFModel_ACC::moveHelper(MSVehicle* const veh, double vPos) const {
   const double oldV = veh->getSpeed(); // save old v for optional acceleration computation
   const double vSafe = MIN2(vPos, veh->processNextStop(vPos)); // process stops
   // we need the acceleration for emission computation;
   //  in this case, we neglect dawdling, nonetheless, using
   //  vSafe does not incorporate speed reduction due to interaction
   //  on lane changing
   const double vMin = getSpeedAfterMaxDecel(oldV);
   const double vMax = MAX2(vMin,
       MIN3(veh->getLane()->getVehicleMaxSpeed(veh), maxNextSpeed(oldV, veh), vSafe));
#ifdef _DEBUG
   //if (vMin > vMax) {
   //    WRITE_WARNING("Maximum speed of vehicle '" + veh->getID() + "' is lower than the minimum speed (min: " + toString(vMin) + ", max: " + toString(vMax) + ").");
   //}
#endif
   return veh->getLaneChangeModel().patchSpeed(vMin, MAX2(vMin, vMax), vMax, *this);

}


double
MSCFModel_ACC::followSpeed(const MSVehicle* const veh, double speed, double gap2pred, double predSpeed, double /* predMaxDecel */, const MSVehicle* const /* pred */) const {
   return _v(veh, gap2pred, speed, predSpeed, MIN2(veh->getLane()->getSpeedLimit(), veh->getMaxSpeed()), true);
}


double
MSCFModel_ACC::stopSpeed(const MSVehicle* const veh, const double speed, double gap) const {
   // NOTE: This allows return of smaller values than minNextSpeed().
   // Only relevant for the ballistic update: We give the argument headway=TS, to assure that
   // the stopping position is approached with a uniform deceleration also for tau!=TS.
   return MIN2(maximumSafeStopSpeed(gap, speed, false, TS), maxNextSpeed(speed, veh));
}


/// @todo update interactionGap logic
double
MSCFModel_ACC::interactionGap(const MSVehicle* const /* veh */, double /* vL */) const {
   /*maximum radar range is ACC is enabled*/
   return 250;
}

double MSCFModel_ACC::accelSpeedContol(double vErr) const {
   // Speed control law
   double sclAccel = MAX2(MIN2(mySpeedControlGain*vErr, myAccel), -myDecel);
   return sclAccel;
}

double MSCFModel_ACC::accelGapControl(const MSVehicle* const veh, const double gap2pred, const double speed, const double predSpeed, double vErr) const {
   // Gap control law
   double gclAccel = 0.0;
   double desSpacing = myheadwayTime * speed;
   // The argument gap2pred does not consider minGap ->  substract minGap!!
   double gap = gap2pred - veh->getVehicleType().getMinGap();
   double spacingErr = gap - desSpacing;
   double deltaVel = predSpeed - speed;


   if (abs(spacingErr) < 0.2 && abs(vErr) < 0.1) {
       gclAccel = myGapControlGainSpeed*deltaVel + myGapControlGainSpace * spacingErr;
   }else {
       gclAccel = myGapClosingControlGainSpeed *deltaVel + myGapClosingControlGainSpace * spacingErr;
   }

   return gclAccel;
}


double
MSCFModel_ACC::_v(const MSVehicle* const veh, const double gap2pred, const double speed,
                  const double predSpeed, const double desSpeed, const bool /* respectMinGap */) const {

   double accelACC = 0;
   double gapLimit_SC = 120; // lower gap limit in meters to enable speed control law
   double gapLimit_GC = 100; // upper gap limit in meters to enable gap control law

   /* Velocity error */
   double vErr = speed - desSpeed;
   int setControlMode = 0;
   ACCVehicleVariables* vars = (ACCVehicleVariables*)veh->getCarFollowVariables();
   if (vars->lastUpdateTime != MSNet::getInstance()->getCurrentTimeStep()) {
       vars->lastUpdateTime = MSNet::getInstance()->getCurrentTimeStep();
       setControlMode = 1;
   }
   if (gap2pred > gapLimit_SC) {
       // Find acceleration - Speed control law
       accelACC = accelSpeedContol(vErr);
       // Set cl to vehicle parameters
       if (setControlMode) vars->ACC_ControlMode = 0;
   } else if (gap2pred < gapLimit_GC) {
       // Find acceleration - Gap control law
       accelACC = accelGapControl(veh, gap2pred, speed, predSpeed, vErr);
       // Set cl to vehicle parameters
       if (setControlMode) vars->ACC_ControlMode = 1;
   } else {
       // Follow previous applied law
       int cm = vars->ACC_ControlMode;
       if (!cm) {
           accelACC = accelSpeedContol(vErr);
       } else {
           accelACC = accelGapControl(veh, gap2pred, speed, predSpeed, vErr);
       }

   }

   double newSpeed = speed + ACCEL2SPEED(accelACC);

   return MAX2(0., newSpeed);
}


MSCFModel*
MSCFModel_ACC::duplicate(const MSVehicleType* vtype) const {
   return new MSCFModel_ACC(vtype, myAccel, myDecel, myEmergencyDecel, myHeadwayTime, mySpeedControlGain, myGapClosingControlGainSpeed, myGapClosingControlGainSpace, myGapControlGainSpeed, myGapControlGainSpace);
}
