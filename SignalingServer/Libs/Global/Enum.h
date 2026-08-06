#pragma once
// 👑 支付中台状态枚举
enum PaymentStatus 
{
    PAY_STATUS_UNPAID                   = 0,                                    // 未支付
    PAY_STATUS_SUCCESS                  = 2,                                    // 支付成功
    PAY_STATUS_REFUNDED                 = 4,                                    // 已全额退款
    PAY_STATUS_CLOSED                   = 10,                                   // 关闭/作废
    PAY_STATUS_ERROR                    = -1                                    // 咱们自定义的本地网络异常状态
};

// =========================================================================
// 👑 本地系统订单业务状态 (对应 t_pay_order 表的 status 字段)
// =========================================================================
enum OrderStatus {
    ORDER_STATUS_PENDING                = 0,                                    // 待支付 (等待用户扫码/倒计时中)
    ORDER_STATUS_PAID                   = 1,                                    // 已支付 (已完成加积分与写流水，彻底闭环)
    ORDER_STATUS_CLOSED                 = -1,                                   // 已关闭/作废 (5分钟超时未付、用户手动取消、全额退款)
    ORDER_STATUS_ANOMALY                = -2                                    // 异常挂起 (终极查单时中台宕机/断网，状态未知，需人工核实)
};