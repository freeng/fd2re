# -*- coding: utf-8 -*-
"""fd2dat — FD2 资源 DAT 现代格式替代读写工具包

种类分析与方案见 README-dat-modern.md；证据链见 docs/architecture.md
§5/§6.5-6.11/§13.4/§13.15/§13.28/§13.33/§13.43/§13.47/§13.77。

命令：
  dat_export.py <DAT|FDICON.B24> [--out DIR] [--palette N]
  dat_pack.py   <导出目录> [--out FILE] [--verify]
  dat_preview.py <导出目录> <子命令 ...>   # 仅用现代格式文件模拟工程使用
"""
__version__ = "1.0"
