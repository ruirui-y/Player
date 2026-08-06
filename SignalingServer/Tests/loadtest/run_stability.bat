@echo off
echo ========================================
echo  DemandStation Boost 稳定性测试
echo  100 用户, 2s 间隔, 120s 持续
echo ========================================
python loadtest.py --users 100 --interval 2 --duration 120
pause
