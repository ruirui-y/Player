@echo off
echo ========================================
echo  DemandStation Boost 冒烟测试
echo  1 用户, 2s 间隔, 30s 持续
echo ========================================
python loadtest.py --users 1 --interval 2 --duration 30
pause
