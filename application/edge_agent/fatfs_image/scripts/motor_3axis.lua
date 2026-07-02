-- ================================================================
--  motor_3axis.lua — 三轴电机控制
--  依赖: espnow_motor C 模块 (require "espnow_motor")
-- ================================================================

local motor = require("espnow_motor")
local SLAVE  = "3c:84:27:c9:30:74"

function motor_axis(axis, deg)
    return motor.axis(SLAVE, axis, deg)
end

function motor_3axis(roll, pitch, yaw)
    return motor.send3(SLAVE, roll, pitch, yaw)
end

function motor_zero()
    return motor.axis(SLAVE, "z", 0)
end

print("motor_3axis.lua loaded")
