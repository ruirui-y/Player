-- ============================================
-- DemandStation 压测账号批量创建
-- 用法: mysql -h127.0.0.1 -P33060 -uroot -p123456 demandstation < seed_users.sql
-- ============================================

-- 创建 1000 个测试用户 (test_user_0 ~ test_user_999)
-- 密码: 123456 (bcrypt hash, 需根据实际加密方式替换)
-- 注意: 如果表结构不同，请先对照实际 schema 调整

DELIMITER $$

CREATE PROCEDURE IF NOT EXISTS seed_test_users(IN n INT)
BEGIN
    DECLARE i INT DEFAULT 0;
    WHILE i < n DO
        INSERT IGNORE INTO t_user (username, password, status, create_time)
        VALUES (
            CONCAT('test_user_', i),
            '$2a$10$placeholder_bcrypt_hash_for_123456',
            1,
            NOW()
        );
        SET i = i + 1;
    END WHILE;
END$$

DELIMITER ;

-- 执行: 创建 1000 个测试用户
CALL seed_test_users(1000);

-- 清理存储过程
DROP PROCEDURE IF EXISTS seed_test_users;

-- 验证
SELECT COUNT(*) AS total_test_users FROM t_user WHERE username LIKE 'test_user_%';
