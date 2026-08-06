@echo off
echo ========================================
echo  DemandStation Boost 极限压测
echo  1000 用户, 0.1s 间隔, 300s 持续
echo  分批 50 人, 间隔 0.2s
echo ========================================
python loadtest.py --users 1000 --interval 0.1 --duration 300 --batch-size 50 --batch-interval 0.2
pause
