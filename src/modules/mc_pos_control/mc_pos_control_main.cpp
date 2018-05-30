/****************************************************************************
 *
 *   Copyright (c) 2013 - 2017 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/**
 * @file mc_pos_control_main.cpp
 * Multicopter position controller.
 *
 * The controller has two loops: P loop for position error and PID loop for velocity error.
 * Output of velocity controller is thrust vector that is split to thrust direction
 * (i.e. rotation matrix for multicopter orientation) and thrust scalar (i.e. multicopter thrust itself).
 * Controller doesn't use Euler angles for work, they generated only for more human-friendly control and logging.
 */

#include <px4_config.h>
#include <px4_defines.h>
#include <px4_module_params.h>
#include <px4_tasks.h>
#include <px4_posix.h>
#include <drivers/drv_hrt.h>
#include <systemlib/hysteresis/hysteresis.h>

#include <uORB/topics/home_position.h>
#include <uORB/topics/parameter_update.h>
#include <uORB/topics/vehicle_attitude_setpoint.h>
#include <uORB/topics/vehicle_control_mode.h>
#include <uORB/topics/vehicle_land_detected.h>
#include <uORB/topics/vehicle_local_position.h>
#include <uORB/topics/vehicle_local_position_setpoint.h>
#include <uORB/topics/vehicle_status.h>

#include <float.h>
#include <mathlib/mathlib.h>
#include <systemlib/mavlink_log.h>

#include <controllib/blocks.hpp>

#include <lib/FlightTasks/FlightTasks.hpp>
#include "PositionControl.hpp"
#include "Utility/ControlMath.hpp"

/**
 * Multicopter position control app start / stop handling function
 *
 * @ingroup apps
 */
extern "C" __EXPORT int mc_pos_control_main(int argc, char *argv[]);

class MulticopterPositionControl : public control::SuperBlock, public ModuleParams
{
public:
	/**
	 * Constructor
	 */
	MulticopterPositionControl();

	/**
	 * Destructor, also kills task.
	 */
	~MulticopterPositionControl();

	/**
	 * Start task.
	 *
	 * @return		OK on success.
	 */
	int		start();

private:

	bool		_task_should_exit = false;			/**<true if task should exit */
	bool 		_in_smooth_takeoff = false; 		/**<true if takeoff ramp is applied */

	orb_advert_t	_mavlink_log_pub{nullptr};		/**< mavlink log advert */
	orb_advert_t	_att_sp_pub{nullptr};			/**< attitude setpoint publication */
	orb_advert_t	_local_pos_sp_pub{nullptr};		/**< vehicle local position setpoint publication */
	orb_id_t _attitude_setpoint_id{nullptr};

	int		_control_task{-1};			/**< task handle for task */
	int		_vehicle_status_sub{-1};		/**< vehicle status subscription */
	int		_vehicle_land_detected_sub{-1};	/**< vehicle land detected subscription */
	int		_control_mode_sub{-1};		/**< vehicle control mode subscription */
	int		_params_sub{-1};			/**< notification of parameter updates */
	int		_local_pos_sub{-1};			/**< vehicle local position */
	int		_home_pos_sub{-1}; 			/**< home position */

	float _takeoff_speed = -1.f; /**< For flighttask interface used only. It can be thrust or velocity setpoints */

	vehicle_status_s 			_vehicle_status{}; 	/**< vehicle status */
	vehicle_land_detected_s 			_vehicle_land_detected{};	/**< vehicle land detected */
	vehicle_attitude_setpoint_s		_att_sp{};		/**< vehicle attitude setpoint */
	vehicle_control_mode_s			_control_mode{};		/**< vehicle control mode */
	vehicle_local_position_s			_local_pos{};		/**< vehicle local position */
	vehicle_local_position_setpoint_s	_local_pos_sp{};		/**< vehicle local position setpoint */
	home_position_s				_home_pos{}; 				/**< home position */

	DEFINE_PARAMETERS(
		(ParamFloat<px4::params::MPC_TKO_RAMP_T>) _takeoff_ramp_time, /**< time constant for smooth takeoff ramp */
		(ParamFloat<px4::params::MPC_Z_VEL_MAX_UP>) _vel_max_up,
		(ParamFloat<px4::params::MPC_Z_VEL_MAX_DN>) _vel_max_down,
		(ParamFloat<px4::params::MPC_LAND_SPEED>) _land_speed,
		(ParamFloat<px4::params::MPC_TKO_SPEED>) _tko_speed,
		(ParamFloat<px4::params::MPC_LAND_ALT2>) MPC_LAND_ALT2, // altitude at which speed limit downwards reached minimum speed
		(ParamInt<px4::params::MPC_POS_MODE>) MPC_POS_MODE
	);

	control::BlockDerivative _vel_x_deriv; /**< velocity derivative in x */
	control::BlockDerivative _vel_y_deriv; /**< velocity derivative in y */
	control::BlockDerivative _vel_z_deriv; /**< velocity derivative in z */

	FlightTasks _flight_tasks; /**< class that generates position controller tracking setpoints*/
	PositionControl _control; /**< class that handles the core PID position controller */
	PositionControlStates _states; /**< structure that contains required state information for position control */

	hrt_abstime _last_warn = 0; /**< timer when the last warn message was sent out */
	static constexpr uint64_t IDLE_BOFORE_TAKEOFF_TIME_US =
		2500000; /**< time required to stay idle before enabling smooth takeoff */

	/**
	 * Hysteresis that turns true once vehicle is armed for IDLE_BOFORE_TAKEOFF_TIME_US microseconds.
	 * A real vehicle requires some time to accelerates the propellers to IDLE speed. To ensure
	 * that the propellers reach idle speed before initiating a takeoff, a delay of IDLE_BOFORE_TAKEOFF_TIME_US
	 * is added.
	 */
	systemlib::Hysteresis _arm_hysteresis{false}; /**< becomes true once vehicle is armed for IDLE_BOFORE_TAKEOFF_TIME_US */

	/**
	 * Update our local parameter cache.
	 * Parameter update can be forced when argument is true.
	 * @param force forces parameter update.
	 */
	int		parameters_update(bool force);

	/**
	 * Check for changes in subscribed topics.
	 */
	void		poll_subscriptions();

	/**
	 * Check for validity of positon/velocity states.
	 * @param vel_sp_z velocity setpoint in z-direction
	 */
	void check_vehicle_states(const float &vel_sp_z);

	/**
	 * Limit altitude based on land-detector.
	 * @param setpoint needed to detect vehicle intention.
	 */
	void limit_altitude(vehicle_local_position_setpoint_s &setpoint);

	/**
	 * Prints a warning message at a lowered rate.
	 * @param str the message that has to be printed.
	 */
	void warn_rate_limited(const char *str);

	/**
	 * Publish attitude.
	 */
	void publish_attitude();

	/**
	 * Publish local position setpoint.
	 * This is only required for logging.
	 */
	void publish_local_pos_sp();

	/**
	 * Checks if smooth takeoff is initiated.
	 * @param position_setpoint_z the position setpoint in the z-Direction
	 * @param velocity setpoint_z the velocity setpoint in the z-Direction
	 */
	void check_for_smooth_takeoff(const float &position_setpoint_z, const float &velocity_setpoint_z,
				      const vehicle_constraints_s &constraints);

	/**
	 * Check if smooth takeoff has ended and updates accordingly.
	 * @param position_setpoint_z the position setpoint in the z-Direction
	 * @param velocity setpoint_z the velocity setpoint in the z-Direction
	 */
	void update_smooth_takeoff(const float &position_setpoint_z, const float &velocity_setpoint_z);

	/**
	 * Adjust the thrust setpoint during landing.
	 * Thrust is adjusted to support the land-detector during detection.
	 * @param thrust_setpoint gets adjusted based on land-detector state
	 */
	void limit_thrust_during_landing(matrix::Vector3f &thrust_sepoint);

	/**
	 * Start flightasks based on navigation state.
	 * This methods activates a taks basedn on the navigation state.
	 */
	void start_flight_task();

	/**
	 * Shim for calling task_main from task_create.
	 */
	static int	task_main_trampoline(int argc, char *argv[]);

	/**
	 * Main sensor collection task.
	 */
	void		task_main();
};

namespace pos_control
{
MulticopterPositionControl	*g_control;
}

MulticopterPositionControl::MulticopterPositionControl() :
	SuperBlock(nullptr, "MPC"),
	ModuleParams(nullptr),
	_vel_x_deriv(this, "VELD"),
	_vel_y_deriv(this, "VELD"),
	_vel_z_deriv(this, "VELD"),
	_control(this)
{
	// fetch initial parameter values
	parameters_update(true);

	// set trigger time for arm hysteresis
	_arm_hysteresis.set_hysteresis_time_from(false, IDLE_BOFORE_TAKEOFF_TIME_US);
}

MulticopterPositionControl::~MulticopterPositionControl()
{
	if (_control_task != -1) {
		// task wakes up every 100ms or so at the longest
		_task_should_exit = true;

		// wait for a second for the task to quit at our request
		unsigned i = 0;

		do {
			// wait 20ms
			usleep(20000);

			// if we have given up, kill it
			if (++i > 50) {
				px4_task_delete(_control_task);
				break;
			}
		} while (_control_task != -1);
	}

	pos_control::g_control = nullptr;
}

void
MulticopterPositionControl::warn_rate_limited(const char *string)
{
	hrt_abstime now = hrt_absolute_time();

	if (now - _last_warn > 200000) {
		PX4_WARN(string);
		_last_warn = now;
	}
}

int
MulticopterPositionControl::parameters_update(bool force)
{
	bool updated;
	struct parameter_update_s param_upd;

	orb_check(_params_sub, &updated);

	if (updated) {
		orb_copy(ORB_ID(parameter_update), _params_sub, &param_upd);
	}

	if (updated || force) {
		ModuleParams::updateParams();
		SuperBlock::updateParams();

		_flight_tasks.handleParameterUpdate();

		/* initialize vectors from params and enforce constraints */
		_tko_speed.set(math::min(_tko_speed.get(), _vel_max_up.get()));
		_land_speed.set(math::min(_land_speed.get(), _vel_max_down.get()));
	}

	return OK;
}

void
MulticopterPositionControl::poll_subscriptions()
{
	bool updated;

	orb_check(_vehicle_status_sub, &updated);

	if (updated) {
		orb_copy(ORB_ID(vehicle_status), _vehicle_status_sub, &_vehicle_status);

		// set correct uORB ID, depending on if vehicle is VTOL or not
		if (!_attitude_setpoint_id) {
			if (_vehicle_status.is_vtol) {
				_attitude_setpoint_id = ORB_ID(mc_virtual_attitude_setpoint);

			} else {
				_attitude_setpoint_id = ORB_ID(vehicle_attitude_setpoint);
			}
		}
	}

	orb_check(_vehicle_land_detected_sub, &updated);

	if (updated) {
		orb_copy(ORB_ID(vehicle_land_detected), _vehicle_land_detected_sub, &_vehicle_land_detected);
	}

	orb_check(_control_mode_sub, &updated);

	if (updated) {
		orb_copy(ORB_ID(vehicle_control_mode), _control_mode_sub, &_control_mode);
	}

	orb_check(_local_pos_sub, &updated);

	if (updated) {
		orb_copy(ORB_ID(vehicle_local_position), _local_pos_sub, &_local_pos);
	}

	orb_check(_home_pos_sub, &updated);

	if (updated) {
		orb_copy(ORB_ID(home_position), _home_pos_sub, &_home_pos);
	}
}

int
MulticopterPositionControl::task_main_trampoline(int argc, char *argv[])
{
	pos_control::g_control->task_main();
	return 0;
}

void
MulticopterPositionControl::limit_altitude(vehicle_local_position_setpoint_s &setpoint)
{
	if (_vehicle_land_detected.alt_max < 0.0f || !_home_pos.valid_alt || !_local_pos.v_z_valid) {
		// there is no altitude limitation present or the required information not available
		return;
	}

	float altitude_above_home = -(_states.position(2) - _home_pos.z);

	if (altitude_above_home > _vehicle_land_detected.alt_max) {
		// we are above maximum altitude
		setpoint.z = -_vehicle_land_detected.alt_max +  _home_pos.z;
		setpoint.vz = 0.0f;

	} else if (setpoint.vz <= 0.0f) {
		// we want to fly upwards: check if vehicle does not exceed altitude

		float delta_p = _vehicle_land_detected.alt_max - altitude_above_home;

		if (fabsf(setpoint.vz) * _dt > delta_p) {
			setpoint.z = -_vehicle_land_detected.alt_max +  _home_pos.z;
			setpoint.vz = 0.0f;
		}
	}
}

void
MulticopterPositionControl::check_vehicle_states(const float &vel_sp_z)
{
	if (_local_pos.timestamp == 0) {
		return;
	}

	// only set position states if valid and finite
	if (PX4_ISFINITE(_local_pos.x) && PX4_ISFINITE(_local_pos.y) && _local_pos.xy_valid) {
		_states.position(0) = _local_pos.x;
		_states.position(1) = _local_pos.y;

	} else {
		_states.position(0) = _states.position(1) = NAN;
	}

	if (PX4_ISFINITE(_local_pos.z) && _local_pos.z_valid) {
		_states.position(2) = _local_pos.z;

	} else {
		_states.position(2) = NAN;
	}

	if (PX4_ISFINITE(_local_pos.vx) && PX4_ISFINITE(_local_pos.vy) && _local_pos.v_xy_valid) {
		_states.velocity(0) = _local_pos.vx;
		_states.velocity(1) = _local_pos.vy;
		_states.acceleration(0) = _vel_x_deriv.update(-_states.velocity(0));
		_states.acceleration(1) = _vel_y_deriv.update(-_states.velocity(1));

	} else {
		_states.velocity(0) = _states.velocity(1) = NAN;
		_states.acceleration(0) = _states.acceleration(1) = NAN;

		// since no valid velocity, update derivate with 0
		_vel_x_deriv.update(0.0f);
		_vel_y_deriv.update(0.0f);
	}

	if (PX4_ISFINITE(_local_pos.vz)) {

		if (PX4_ISFINITE(vel_sp_z) && fabsf(vel_sp_z) > FLT_EPSILON && PX4_ISFINITE(_local_pos.z_deriv)) {
			// A change in velocity is demanded. Set velocity to the derivative of position
			// because it has less bias but blend it in across the landing speed range
			float weighting = fminf(fabsf(vel_sp_z) / _land_speed.get(), 1.0f);
			_states.velocity(2) = _local_pos.z_deriv * weighting + _local_pos.vz * (1.0f - weighting);
		}

		_states.velocity(2) = _local_pos.vz;
		_states.acceleration(2) = _vel_z_deriv.update(-_states.velocity(2));

	} else {
		_states.velocity(2) = _states.acceleration(2) = NAN;
		// since no valid velocity, update derivate with 0
		_vel_z_deriv.update(0.0f);

	}

	if (PX4_ISFINITE(_local_pos.yaw)) {
		_states.yaw = _local_pos.yaw;
	}
}

void
MulticopterPositionControl::task_main()
{
	// do subscriptions
	_vehicle_status_sub = orb_subscribe(ORB_ID(vehicle_status));
	_vehicle_land_detected_sub = orb_subscribe(ORB_ID(vehicle_land_detected));
	_control_mode_sub = orb_subscribe(ORB_ID(vehicle_control_mode));
	_params_sub = orb_subscribe(ORB_ID(parameter_update));
	_local_pos_sub = orb_subscribe(ORB_ID(vehicle_local_position));
	_home_pos_sub = orb_subscribe(ORB_ID(home_position));

	parameters_update(true);

	// get an initial update for all sensor and status data
	poll_subscriptions();

	// We really need to know from the beginning if we're landed or in-air.
	orb_copy(ORB_ID(vehicle_land_detected), _vehicle_land_detected_sub, &_vehicle_land_detected);

	hrt_abstime t_prev = 0;

	// Let's be safe and have the landing gear down by default
	_att_sp.landing_gear = vehicle_attitude_setpoint_s::LANDING_GEAR_DOWN;

	// wakeup source
	px4_pollfd_struct_t fds[1];

	fds[0].fd = _local_pos_sub;
	fds[0].events = POLLIN;

	while (!_task_should_exit) {
		// wait for up to 20ms for data
		int pret = px4_poll(&fds[0], (sizeof(fds) / sizeof(fds[0])), 20);

		// timed out - periodic check for _task_should_exit
		if (pret == 0) {
			// Go through the loop anyway to copy manual input at 50 Hz.
		}

		// this is undesirable but not much we can do
		if (pret < 0) {
			warn("poll error %d, %d", pret, errno);
			continue;
		}

		poll_subscriptions();

		parameters_update(false);

		hrt_abstime t = hrt_absolute_time();
		const float dt = t_prev != 0 ? (t - t_prev) / 1e6f : 0.004f;
		t_prev = t;

		// set dt for control blocks
		setDt(dt);

		if (_control_mode.flag_armed) {
			start_flight_task();

		} else {
			// disable flighttask
			_flight_tasks.switchTask(FlightTaskIndex::None);
			// reset arm hysteresis
			_arm_hysteresis.set_state_and_update(false);
		}

		// check if any task is active
		if (_flight_tasks.isAnyTaskActive()) {

			// setpoints from flighttask
			vehicle_local_position_setpoint_s setpoint;

			// update task
			if (!_flight_tasks.update()) {
				// Task was not able to update correctly. Do Failsafe.
				setpoint.x = setpoint.y = setpoint.z = NAN;
				setpoint.vx = setpoint.vy = setpoint.vz = NAN;
				setpoint.thrust[0] = setpoint.thrust[1] = setpoint.thrust[2] = NAN;

				if (PX4_ISFINITE(_states.velocity(2))) {
					// We have a valid velocity in D-direction.
					// descend downwards with landspeed.
					setpoint.vz = _land_speed.get();
					setpoint.thrust[0] = setpoint.thrust[1] = 0.0f;
					warn_rate_limited("Failsafe: Descend with land-speed.");

				} else {
					// Use the failsafe from the PositionController.
					warn_rate_limited("Failsafe: Descend with just attitude control.");
				}

			} else {
				setpoint = _flight_tasks.getPositionSetpoint();
			}

			vehicle_constraints_s constraints = _flight_tasks.getConstraints();

			// check if all local states are valid and map accordingly
			check_vehicle_states(setpoint.vz);

			// We can only run the control if we're already in-air, have a takeoff setpoint, and are not
			// in pure manual and vehicle is armed for some time. Otherwise just stay idle.
			_arm_hysteresis.set_state_and_update(_control_mode.flag_armed);

			// we can only do a smooth takeoff if a valid velocity or position is available and are
			// armed long enough
			if (_arm_hysteresis.get_state() && PX4_ISFINITE(_states.position(2)) && PX4_ISFINITE(_states.velocity(2))) {
				check_for_smooth_takeoff(setpoint.z, setpoint.vz, constraints);
				update_smooth_takeoff(setpoint.z, setpoint.vz);

				if (_in_smooth_takeoff) {
					constraints.speed_up = _takeoff_speed;
					// during smooth takeoff we disable yaw command
					setpoint.yaw = setpoint.yawspeed = NAN;
					// don't control position in xy
					setpoint.x = setpoint.y = NAN;
					setpoint.vx = setpoint.vy = 0.0f;
				}
			}

			if (_vehicle_land_detected.landed && !_in_smooth_takeoff && !PX4_ISFINITE(setpoint.thrust[2])) {
				// Keep throttle low
				setpoint.thrust[0] = setpoint.thrust[1] = setpoint.thrust[2] = 0.0f;
				setpoint.x = setpoint.y = setpoint.z = NAN;
				setpoint.vx = setpoint.vy = setpoint.vz = NAN;
				setpoint.yawspeed = NAN;
				setpoint.yaw = _states.yaw;
				constraints.landing_gear = vehicle_constraints_s::GEAR_KEEP;
			}

			// limit altitude only if local position is valid
			if (PX4_ISFINITE(_states.position(2))) {limit_altitude(setpoint);}

			// Update states, setpoints and constraints.
			_control.updateConstraints(constraints);
			_control.updateState(_states);
			_control.updateSetpoint(setpoint);

			// Generate desired thrust and yaw.
			_control.generateThrustYawSetpoint(_dt);

			matrix::Vector3f thr_sp = _control.getThrustSetpoint();

			// Adjust thrust setpoint based on landdetector only if the
			// vehicle is NOT in pure Manual mode.
			if (!_in_smooth_takeoff && !PX4_ISFINITE(setpoint.thrust[2])) {limit_thrust_during_landing(thr_sp);}

			// Fill local position, velocity and thrust setpoint.
			_local_pos_sp.timestamp = hrt_absolute_time();
			_local_pos_sp.x = _control.getPosSp()(0);
			_local_pos_sp.y = _control.getPosSp()(1);
			_local_pos_sp.z = _control.getPosSp()(2);
			_local_pos_sp.yaw = _control.getYawSetpoint();
			_local_pos_sp.yawspeed = _control.getYawspeedSetpoint();

			_local_pos_sp.vx = _control.getVelSp()(0);
			_local_pos_sp.vy = _control.getVelSp()(1);
			_local_pos_sp.vz = _control.getVelSp()(2);
			thr_sp.copyTo(_local_pos_sp.thrust);

			// Fill attitude setpoint. Attitude is computed from yaw and thrust setpoint.
			_att_sp = ControlMath::thrustToAttitude(thr_sp, _control.getYawSetpoint());
			_att_sp.yaw_sp_move_rate = _control.getYawspeedSetpoint();
			_att_sp.fw_control_yaw = false;
			_att_sp.disable_mc_yaw_control = false;
			_att_sp.apply_flaps = false;

			if (!constraints.landing_gear) {
				if (constraints.landing_gear == vehicle_constraints_s::GEAR_UP) {
					_att_sp.landing_gear = vehicle_attitude_setpoint_s::LANDING_GEAR_UP;
				}

				if (constraints.landing_gear == vehicle_constraints_s::GEAR_DOWN) {
					_att_sp.landing_gear = vehicle_attitude_setpoint_s::LANDING_GEAR_DOWN;
				}
			}

			// Publish local position setpoint (for logging only) and attitude setpoint (for attitude controller).
			publish_local_pos_sp();

		} else {

			// no flighttask is active: stay idle
			_att_sp.roll_body = _att_sp.pitch_body = 0.0f;
			_att_sp.yaw_body = _local_pos.yaw;
			_att_sp.yaw_sp_move_rate = 0.0f;
			_att_sp.fw_control_yaw = false;
			_att_sp.disable_mc_yaw_control = false;
			_att_sp.apply_flaps = false;
			matrix::Quatf q_sp = matrix::Eulerf(_att_sp.roll_body, _att_sp.pitch_body, _att_sp.yaw_body);
			q_sp.copyTo(_att_sp.q_d);
			_att_sp.q_d_valid = true;
			_att_sp.thrust = 0.0f;
		}

		publish_attitude();
	}

	mavlink_log_info(&_mavlink_log_pub, "[mpc] stopped");

	_control_task = -1;
}

void
MulticopterPositionControl::start_flight_task()
{
	bool task_failure = false;

	// offboard
	if (_vehicle_status.nav_state == vehicle_status_s::NAVIGATION_STATE_OFFBOARD) {
		int error = _flight_tasks.switchTask(FlightTaskIndex::Offboard);

		if (error != 0) {
			PX4_WARN("Offboard activation failded with error: %s", _flight_tasks.errorToString(error));
			task_failure = true;
		}
	}

	// Auto-follow me
	if (_vehicle_status.nav_state == vehicle_status_s::NAVIGATION_STATE_AUTO_FOLLOW_TARGET) {
		int error = _flight_tasks.switchTask(FlightTaskIndex::AutoFollowMe);

		if (error != 0) {
			PX4_WARN("Follow-Me activation failed with error: %s", _flight_tasks.errorToString(error));
			task_failure = true;
		}

	} else if (_control_mode.flag_control_auto_enabled) {
		// Auto relate maneuvers
		int error = _flight_tasks.switchTask(FlightTaskIndex::AutoLine);

		if (error != 0) {
			PX4_WARN("Auto activation failed with error: %s", _flight_tasks.errorToString(error));
			task_failure = true;
		}
	}

	// manual position control
	if (_vehicle_status.nav_state == vehicle_status_s::NAVIGATION_STATE_POSCTL || task_failure) {

		int error = 0;

		switch (MPC_POS_MODE.get()) {
		case 0:
			error =  _flight_tasks.switchTask(FlightTaskIndex::Position);
			break;

		case 1:
			error =  _flight_tasks.switchTask(FlightTaskIndex::PositionSmooth);
			break;

		case 2:
			error =  _flight_tasks.switchTask(FlightTaskIndex::Sport);
			break;

		default:
			error =  _flight_tasks.switchTask(FlightTaskIndex::Position);
			break;
		}

		if (error != 0) {
			PX4_WARN("Position-Ctrl activation failed with error: %s", _flight_tasks.errorToString(error));
			task_failure = true;

		} else {
			task_failure = false;
		}
	}

	// manual altitude control
	if (_vehicle_status.nav_state == vehicle_status_s::NAVIGATION_STATE_ALTCTL || task_failure) {
		int error = _flight_tasks.switchTask(FlightTaskIndex::Altitude);

		if (error != 0) {
			PX4_WARN("Altitude-Ctrl activation failed with error: %s", _flight_tasks.errorToString(error));
			task_failure = true;

		} else {
			task_failure = false;
		}
	}


	// manual stabilized control
	if (_vehicle_status.nav_state == vehicle_status_s::NAVIGATION_STATE_MANUAL
	    ||  _vehicle_status.nav_state == vehicle_status_s::NAVIGATION_STATE_STAB || task_failure) {
		int error = _flight_tasks.switchTask(FlightTaskIndex::Stabilized);

		if (error != 0) {
			PX4_WARN("Stabilized-Ctrl failed with error: %s", _flight_tasks.errorToString(error));
			task_failure = true;

		} else {
			task_failure = false;
		}
	}

	// check task failure
	if (task_failure) {
		// No task was activated.
		_flight_tasks.switchTask(FlightTaskIndex::None);
		warn_rate_limited("No Flighttask is running");
	}

}

void
MulticopterPositionControl::check_for_smooth_takeoff(const float &z_sp, const float &vz_sp,
		const vehicle_constraints_s &constraints)
{
	// Check for smooth takeoff
	if (_vehicle_land_detected.landed && !_in_smooth_takeoff) {
		// Vehicle is still landed and no takeoff was initiated yet.
		// Adjust for different takeoff cases.
		// The minimum takeoff altitude needs to be at least 20cm above minimum distance or, if valid, above minimum distance
		// above ground.
		float min_altitude = PX4_ISFINITE(constraints.min_distance_to_ground) ? (constraints.min_distance_to_ground + 0.05f) :
				     0.2f;

		if ((PX4_ISFINITE(z_sp) && z_sp < _states.position(2) - min_altitude) ||
		    (PX4_ISFINITE(vz_sp) && vz_sp < math::min(-_tko_speed.get(), -0.6f))) {
			// There is a position setpoint above current position or velocity setpoint larger than
			// takeoff speed. Enable smooth takeoff.
			_in_smooth_takeoff = true;
			_takeoff_speed = -0.5f;

		} else {
			// Default
			_in_smooth_takeoff = false;
		}
	}
}

void
MulticopterPositionControl::update_smooth_takeoff(const float &z_sp, const float &vz_sp)
{
	// If in smooth takeoff, adjust setpoints based on what is valid:
	// 1. position setpoint is valid -> go with takeoffspeed to specific altitude
	// 2. position setpoint not valid but velocity setpoint valid: ramp up velocity
	if (_in_smooth_takeoff) {
		float desired_tko_speed = -vz_sp;

		// If there is a valid position setpoint, then set the desired speed to the takeoff speed.
		if (PX4_ISFINITE(z_sp)) {
			desired_tko_speed = _tko_speed.get();
		}

		// Ramp up takeoff speed.
		_takeoff_speed += desired_tko_speed * _dt / _takeoff_ramp_time.get();
		_takeoff_speed = math::min(_takeoff_speed, desired_tko_speed);

		// Smooth takeoff is achieved once desired altitude/velocity setpoint is reached.
		if (PX4_ISFINITE(z_sp)) {
			_in_smooth_takeoff = _states.position(2) - 0.2f > math::max(z_sp, -MPC_LAND_ALT2.get());

		} else  {
			_in_smooth_takeoff = _takeoff_speed < -vz_sp;
		}

	} else {
		_in_smooth_takeoff = false;
	}
}

void
MulticopterPositionControl::limit_thrust_during_landing(matrix::Vector3f &thr_sp)
{
	if (_vehicle_land_detected.ground_contact) {
		// Set thrust in xy to zero
		thr_sp(0) = 0.0f;
		thr_sp(1) = 0.0f;
		// Reset integral in xy is required because PID-controller does
		// know about the overwrite and would therefore increase the intragral term
		_control.resetIntegralXY();
	}

	if (_vehicle_land_detected.maybe_landed) {
		// we set thrust to zero
		// this will help to decide if we are actually landed or not
		thr_sp.zero();
		// We need to reset all integral terms otherwise the PID-controller
		// will continue to integrate
		_control.resetIntegralXY();
		_control.resetIntegralZ();
	}
}

void
MulticopterPositionControl::publish_attitude()
{
	// publish attitude setpoint
	// Do not publish if
	// - offboard is enabled but position/velocity/accel control is disabled,
	// in this case the attitude setpoint is published by the mavlink app.
	// - if the vehicle is a VTOL and it's just doing a transition (the VTOL attitude control module will generate
	// attitude setpoints for the transition).
	// - if not armed
	//
	if (_arm_hysteresis.get_state() &&
	    (!(_control_mode.flag_control_offboard_enabled &&
	       !(_control_mode.flag_control_position_enabled ||
		 _control_mode.flag_control_velocity_enabled ||
		 _control_mode.flag_control_acceleration_enabled)))) {

		_att_sp.timestamp = hrt_absolute_time();

		if (_att_sp_pub != nullptr) {
			orb_publish(_attitude_setpoint_id, _att_sp_pub, &_att_sp);

		} else if (_attitude_setpoint_id) {
			_att_sp_pub = orb_advertise(_attitude_setpoint_id, &_att_sp);
		}
	}
}

void
MulticopterPositionControl::publish_local_pos_sp()
{

	_local_pos_sp.timestamp = hrt_absolute_time();

	// publish local position setpoint
	if (_local_pos_sp_pub != nullptr) {
		orb_publish(ORB_ID(vehicle_local_position_setpoint),
			    _local_pos_sp_pub, &_local_pos_sp);

	} else {
		_local_pos_sp_pub = orb_advertise(
					    ORB_ID(vehicle_local_position_setpoint),
					    &_local_pos_sp);
	}
}

int
MulticopterPositionControl::start()
{
	// start the task
	_control_task = px4_task_spawn_cmd("mc_pos_control",
					   SCHED_DEFAULT,
					   SCHED_PRIORITY_POSITION_CONTROL,
					   1900,
					   (px4_main_t)&MulticopterPositionControl::task_main_trampoline,
					   nullptr);

	if (_control_task < 0) {
		warn("task start failed");
		return -errno;
	}

	return OK;
}

int mc_pos_control_main(int argc, char *argv[])
{
	if (argc < 2) {
		warnx("usage: mc_pos_control {start|stop|status}");
		return 1;
	}

	if (!strcmp(argv[1], "start")) {

		if (pos_control::g_control != nullptr) {
			warnx("already running");
			return 1;
		}

		pos_control::g_control = new MulticopterPositionControl;

		if (pos_control::g_control == nullptr) {
			warnx("alloc failed");
			return 1;
		}

		if (OK != pos_control::g_control->start()) {
			delete pos_control::g_control;
			pos_control::g_control = nullptr;
			warnx("start failed");
			return 1;
		}

		return 0;
	}

	if (!strcmp(argv[1], "stop")) {
		if (pos_control::g_control == nullptr) {
			warnx("not running");
			return 1;
		}

		delete pos_control::g_control;
		pos_control::g_control = nullptr;
		return 0;
	}

	if (!strcmp(argv[1], "status")) {
		if (pos_control::g_control) {
			warnx("running");
			return 0;

		} else {
			warnx("not running");
			return 1;
		}
	}

	warnx("unrecognized command");
	return 1;
}
