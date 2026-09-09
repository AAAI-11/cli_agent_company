#!/usr/bin/env python3
"""text 短标签字库符号表生成器（可复现管线的一环）。

用法：
  python3 tools/gen_font_text.py --symbols     打印去重后的符号串（喂给 lv_font_conv --symbols）
  python3 tools/gen_font_text.py --header      生成 main/font_text_symbols.h（宿主测试锚定覆盖）

字体再生成（README v0.3 章节同款命令）：
  npx --yes lv_font_conv --no-compress --no-prefilter --bpp 1 --size 24 \\
    --font /tmp/DroidSansFallback.ttf -r 0x20-0x7F \\
    --symbols "$(python3 tools/gen_font_text.py --symbols)" \\
    --format lvgl --force-fast-kern-format -o main/font_pet_text_24.c

选字策略（用户拍板：常用 ~500 字折中，生僻字显 '?'）：
高频通用字 + 本项目演示高频词（工具/协议/开发/答辩语境）。
改动字表后必须重跑 --header 并重新生成字体，两处以本脚本为唯一源头。
"""
import sys
from pathlib import Path

# 高频通用字（按现代汉语字频序的前段取常用）
FREQ = (
    "的一是了我不人在他有这上们来到时大地为子中你说生国年着就那和要出也"
    "得里后自以会家可下而过天去能对小多然于心学之都好看起发当没成只如"
    "事把还用第样道想作种开美总从无情己面最女但现前些所同日手又行意动"
    "方期它头经长儿回位分爱老因很给名法间斯知世什两次使身者被高已亲其"
    "进此话常与活正感见明问力理尔点文几定本公特做外孩相西果走将月十实"
    "向声车全信重三机工物气每并别真打太新比才便夫再书部水像眼等体却加"
    "电主界门利海受听表德少克代员先口由死安写性马光白或住难望教命花结"
    "乐色更拉东神记处让母父应直字场平报友关放至张认接告入笑内军候民岁"
    "往何度山觉路带万男边风解叫任金快原吃妈变通师立象数四失满战远格士"
    "音轻目条呢病始达深完今提求清王化空业思切怎非找片罗钱语元喜曾离飞"
    "科言干流欢约各即指合反题必该论交终林请医晚制球决传画保读运及则房"
    "早院量苦火布品近坐产答星精视五连司巴奇管类未朋且台背油察绝状"
)

# 本项目语境高频词（六态标签 + 工具/协议/开发/演示答辩词汇）
DOMAIN = (
    "思考中干活等你拍板完成了出错正在连接"
    "梳理需求边界请求构建镜像模拟器宠物矢量素材资源形象管理双槽干净室"
    "文件目录代码编译烧录屏幕按键协议链路执行运行查看删除创建修改打开"
    "等待响应就绪验证检验"
    "关闭保存搜索复制粘贴移动重命名测试调试部署安装启动停止重试成功失"
    "败问题错误警告提示确认取消返回刷新输入输出设备服务器客户网络端口"
    "地址数据日志版本分支合并提交推送拉取仓库权限认证加密脚本命令参数"
    "变量函数数组指线程进程队列缓冲驱动固件硬件软件平台架构接口模块组"
    "件依赖库框架模板实例性能优化压缩图像音频视频解码像素分辨率横竖背"
    "景颜色黑白红绿蓝黄紫灰亮暗个只条张次分秒时天周月年浏览器网页面版"
    "上下左右居中齐距用户系统光标终端会话代理智能助手嗯好的继续开始结"
    "束暂停并串口芯片电平复用拉伸裁剪拒绝允许通过撤销授权决策短按长实"
    "体轻触排座载内存分区表排序过滤去重映射轮播闪标呼吸起伏抖动汗滴星"
    "星环绕导入出传下载同步主备延迟频率周期稳定实时预算超泄漏暴露风险"
    "安全证书枚举断言覆盖枚边界回退兜底迭代里程碑答辩演示录屏备份拆包"
    "禁止环境配置缺省阈值区间标点符号路径注释原文"
)

ALL = FREQ + DOMAIN


def symbols() -> str:
    out = []
    seen = set()
    for ch in ALL:
        if ch not in seen and not ch.isspace():
            seen.add(ch)
            out.append(ch)
    s = "".join(out)
    assert 300 <= len(s) <= 700, f"字表规模异常: {len(s)}（预期 300-700）"
    return s


def header() -> str:
    s = symbols()
    return (
        "/**\n"
        " * text 短标签字体符号集（tools/gen_font_text.py 生成，勿手改）。\n"
        " * 必须与 main/font_pet_text_24.c 生成时的 --symbols 逐字一致；\n"
        " * 宿主测试用它锚定 sample_text 覆盖（缺字→屏显 '?'，OLED 时代老坑）。\n"
        " * ASCII 0x20-0x7F 由 lv_font_conv 的 -r 覆盖，不在此列。\n"
        " */\n"
        '#ifndef PET_FONT_TEXT_SYMBOLS_H\n'
        '#define PET_FONT_TEXT_SYMBOLS_H\n\n'
        f'#define PET_FONT_TEXT_SYMBOLS "{s}"\n\n'
        '#endif /* PET_FONT_TEXT_SYMBOLS_H */\n'
    )


if __name__ == "__main__":
    if "--symbols" in sys.argv:
        print(symbols())
    elif "--header" in sys.argv:
        p = Path(__file__).resolve().parent.parent / "main" / "font_text_symbols.h"
        p.write_text(header(), encoding="utf-8")
        print(f"已生成 {p}（{len(symbols())} 字）")
    else:
        print(__doc__)
        sys.exit(1)
