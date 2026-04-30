#!/usr/bin/env python3
"""画一只非常可爱的治愈系小狗动画角色"""

from PIL import Image, ImageDraw
import math
import os

def draw_cute_puppy():
    img = Image.new("RGBA", (400, 500), (255, 255, 255, 0))
    draw = ImageDraw.Draw(img)

    cx, cy = 200, 280  # 身体中心

    # === 身体（圆润的奶油色椭圆） ===
    body_color = (255, 220, 185, 255)  # 奶油色
    draw.ellipse([cx-90, cy-40, cx+90, cy+100], fill=body_color, outline=(230, 180, 140, 255), width=3)

    # === 肚子（浅色椭圆） ===
    belly_color = (255, 240, 220, 255)
    draw.ellipse([cx-55, cy+5, cx+55, cy+80], fill=belly_color)

    # === 后腿（左 + 右） ===
    leg_color = (255, 220, 185, 255)
    # 左后腿
    draw.ellipse([cx-75, cy+80, cx-35, cy+125], fill=leg_color, outline=(230, 180, 140, 255), width=2)
    # 左脚掌
    draw.ellipse([cx-78, cy+115, cx-32, cy+135], fill=(255, 200, 160, 255), outline=(230, 180, 140, 255), width=2)
    # 右后腿
    draw.ellipse([cx+35, cy+80, cx+75, cy+125], fill=leg_color, outline=(230, 180, 140, 255), width=2)
    # 右脚掌
    draw.ellipse([cx+32, cy+115, cx+78, cy+135], fill=(255, 200, 160, 255), outline=(230, 180, 140, 255), width=2)

    # === 尾巴（小毛球，卷卷的） ===
    tail_color = (255, 220, 185, 255)
    draw.ellipse([cx-120, cy-10, cx-90, cy+25], fill=tail_color, outline=(230, 180, 140, 255), width=2)

    # === 头（大圆，比例偏大才可爱） ===
    head_cx, head_cy = cx, cy-95
    head_color = (255, 220, 185, 255)
    draw.ellipse([head_cx-75, head_cy-70, head_cx+75, head_cy+60], fill=head_color, outline=(230, 180, 140, 255), width=3)

    # === 耳朵（大垂耳，很萌） ===
    ear_color = (230, 180, 140, 255)
    ear_inner = (210, 160, 120, 255)
    # 左耳
    draw.ellipse([head_cx-90, head_cy-90, head_cx-40, head_cy-20], fill=ear_color, outline=(190, 140, 100, 255), width=2)
    draw.ellipse([head_cx-85, head_cy-80, head_cx-45, head_cy-30], fill=ear_inner)
    # 右耳
    draw.ellipse([head_cx+40, head_cy-90, head_cx+90, head_cy-20], fill=ear_color, outline=(190, 140, 100, 255), width=2)
    draw.ellipse([head_cx+45, head_cy-80, head_cx+85, head_cy-30], fill=ear_inner)

    # === 眼睛（大眼萌，高光） ===
    # 左眼白
    draw.ellipse([head_cx-30, head_cy-35, head_cx-5, head_cy-8], fill=(255, 255, 255, 255), outline=(100, 70, 50, 255), width=2)
    # 左瞳孔
    draw.ellipse([head_cx-18, head_cy-30, head_cx-10, head_cy-14], fill=(80, 50, 30, 255))
    # 左高光（大）
    draw.ellipse([head_cx-17, head_cy-28, head_cx-12, head_cy-22], fill=(255, 255, 255, 255))
    # 左高光（小）
    draw.ellipse([head_cx-12, head_cy-19, head_cx-10, head_cy-17], fill=(255, 255, 255, 255))

    # 右眼白
    draw.ellipse([head_cx+5, head_cy-35, head_cx+30, head_cy-8], fill=(255, 255, 255, 255), outline=(100, 70, 50, 255), width=2)
    # 右瞳孔
    draw.ellipse([head_cx+10, head_cy-30, head_cx+18, head_cy-14], fill=(80, 50, 30, 255))
    # 右高光（大）
    draw.ellipse([head_cx+12, head_cy-28, head_cx+17, head_cy-22], fill=(255, 255, 255, 255))
    # 右高光（小）
    draw.ellipse([head_cx+10, head_cy-19, head_cx+12, head_cy-17], fill=(255, 255, 255, 255))

    # === 眉毛（小小的，弯弯的） ===
    brow_color = (160, 120, 80, 255)
    # 左眉
    draw.arc([head_cx-32, head_cy-48, head_cx-3, head_cy-32], start=200, end=340, fill=brow_color, width=3)
    # 右眉
    draw.arc([head_cx+3, head_cy-48, head_cx+32, head_cy-32], start=200, end=340, fill=brow_color, width=3)

    # === 鼻子（小爱心/椭圆） ===
    nose_color = (180, 120, 100, 255)
    draw.ellipse([head_cx-8, head_cy-8, head_cx+8, head_cy+4], fill=nose_color, outline=(140, 90, 70, 255), width=2)
    # 鼻头高光
    draw.ellipse([head_cx-3, head_cy-6, head_cx+1, head_cy-3], fill=(220, 180, 160, 255))

    # === 嘴巴（微笑，W形） ===
    mouth_color = (180, 90, 80, 255)
    draw.arc([head_cx-20, head_cy-2, head_cx-2, head_cy+15], start=20, end=160, fill=mouth_color, width=2)
    draw.arc([head_cx+2, head_cy-2, head_cx+20, head_cy+15], start=20, end=160, fill=mouth_color, width=2)

    # === 腮红（超可爱粉晕） ===
    blush_color = (255, 180, 180, 100)
    draw.ellipse([head_cx-50, head_cy-5, head_cx-25, head_cy+15], fill=blush_color)
    draw.ellipse([head_cx+25, head_cy-5, head_cx+50, head_cy+15], fill=blush_color)

    # === 前腿（短萌） ===
    # 左前腿
    draw.ellipse([cx-65, cy+15, cx-25, cy+55], fill=leg_color, outline=(230, 180, 140, 255), width=2)
    draw.ellipse([cx-68, cy+45, cx-22, cy+65], fill=(255, 200, 160, 255), outline=(230, 180, 140, 255), width=2)
    # 右前腿
    draw.ellipse([cx+25, cy+15, cx+65, cy+55], fill=leg_color, outline=(230, 180, 140, 255), width=2)
    draw.ellipse([cx+22, cy+45, cx+68, cy+65], fill=(255, 200, 160, 255), outline=(230, 180, 140, 255), width=2)

    # === 爱心装饰（飘浮的小爱心） ===
    hearts = [
        (60, 60, 1.0),   # (x, y, scale)
        (330, 45, 0.7),
        (100, 20, 0.5),
        (320, 130, 0.6),
        (50, 160, 0.4),
    ]
    for hx, hy, hs in hearts:
        s = 18 * hs
        points = []
        for t in range(0, 360, 5):
            rad = math.radians(t)
            x = s * 16 * (math.sin(rad) ** 3)
            y = -s * (13 * math.cos(rad) - 5 * math.cos(2*rad) - 2 * math.cos(3*rad) - math.cos(4*rad))
            points.append((hx + x, hy + y))
        heart_color = (255, 100, 120, 180) if hs > 0.6 else (255, 150, 170, 160)
        draw.polygon(points, fill=heart_color)

    # === 头顶小呆毛 ===
    tuft_color = (255, 220, 185, 255)
    draw.ellipse([head_cx-8, head_cy-78, head_cx+8, head_cy-62], fill=tuft_color, outline=(230, 180, 140, 255), width=2)
    draw.ellipse([head_cx-3, head_cy-82, head_cx+3, head_cy-70], fill=tuft_color)

    return img

def draw_blink_frame(base_img, frame_num, total_frames=12):
    """生成闭眼帧（用于眨眼动画）"""
    img = base_img.copy()
    draw = ImageDraw.Draw(img)

    head_cx, head_cy = 200, 185  # 头的中心

    # 在第6-8帧闭眼（中间3帧）
    if frame_num in [6, 7, 8]:
        # 用肤色覆盖眼睛
        head_color = (255, 220, 185, 255)
        # 覆盖左眼
        draw.ellipse([head_cx-30, head_cy-35, head_cx-5, head_cy-8], fill=head_color, outline=(100, 70, 50, 255), width=2)
        # 覆盖右眼
        draw.ellipse([head_cx+5, head_cy-35, head_cx+30, head_cy-8], fill=head_color, outline=(100, 70, 50, 255), width=2)
        # 画闭眼线（弯弯的）
        draw.arc([head_cx-30, head_cy-30, head_cx-5, head_cy-10], start=0, end=180, fill=(80, 50, 30, 255), width=3)
        draw.arc([head_cx+5, head_cy-30, head_cx+30, head_cy-10], start=0, end=180, fill=(80, 50, 30, 255), width=3)

    return img

def draw_tail_wag_frame(base_img, frame_num, total_frames=12):
    """尾巴摇摆动画"""
    img = base_img.copy()
    draw = ImageDraw.Draw(img)

    # 尾巴摆动角度
    angle = math.sin(2 * math.pi * frame_num / total_frames) * 20
    rad = math.radians(angle)

    # 尾巴位置 - 先清除旧的尾巴
    tail_color = (255, 220, 185, 255)

    # 摆动后的尾巴
    base_x, base_y = 90, 270  # 尾巴根部
    tx = base_x - 20 * math.cos(rad)
    ty = base_y - 25 * math.sin(rad) - 5
    draw.ellipse([int(tx-15), int(ty-12), int(tx+10), int(ty+12)], fill=tail_color, outline=(230, 180, 140, 255), width=2)

    return img

def create_animation():
    base_img = draw_cute_puppy()
    total_frames = 12
    frames = []

    for i in range(total_frames):
        # 先应用眨眼
        img = draw_blink_frame(base_img, i, total_frames)
        # 再应用尾巴摇摆
        # img = draw_tail_wag_frame(img, i, total_frames)
        frames.append(img)

    save_path = "/home/ubuntu/intchains/test/packages/components/cute_puppy.png"
    # 保存第一帧作为静态图
    frames[0].save(save_path)
    print(f"静态图已保存: {save_path}")

    # 保存GIF动画
    gif_path = "/home/ubuntu/intchains/test/packages/components/cute_puppy.gif"
    frames[0].save(
        gif_path,
        save_all=True,
        append_images=frames[1:],
        duration=150,  # 每帧150ms
        loop=0,
        disposal=2,
        optimize=False,
    )
    print(f"GIF动画已保存: {gif_path}")

    return save_path, gif_path

if __name__ == "__main__":
    png_path, gif_path = create_animation()

    print("\n🎨 已为你绘制了一只超治愈的奶油色小狗！")
    print("   特征：大垂耳、圆眼睛（会眨眼）、粉腮红、小爱心")
    print(f"   静态图: {png_path}")
    print(f"   动  画: {gif_path}")
