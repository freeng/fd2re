# fd2re — 骨架构建（宿主验证用；DOS 目标另行交叉编译）
.PHONY: all clean

all:
	powershell -ExecutionPolicy Bypass -File build.ps1

clean:
	powershell -NoProfile -Command "Remove-Item -Recurse -Force build -ErrorAction SilentlyContinue; Remove-Item -Force fd2re.exe -ErrorAction SilentlyContinue"
