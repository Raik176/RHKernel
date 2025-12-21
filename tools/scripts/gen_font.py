import sys
import os
from PIL import Image, ImageFont, ImageDraw

def generate_font_bin(ttf_path, output_path):
    CHAR_WIDTH = 8
    CHAR_HEIGHT = 16
    
    if not os.path.exists(ttf_path):
        print(f"Error: Font file {ttf_path} not found.")
        sys.exit(1)

    try:
        font = ImageFont.truetype(ttf_path, 14)
    except Exception as e:
        print(f"Error loading font: {e}")
        sys.exit(1)

    with open(output_path, "wb") as f:
        for i in range(256):
            img = Image.new('1', (CHAR_WIDTH, CHAR_HEIGHT), 0)
            draw = ImageDraw.Draw(img)
            
            char = chr(i)
            bbox = draw.textbbox((0, 0), char, font=font)
            w = bbox[2] - bbox[0]
            draw.text(((CHAR_WIDTH - w) // 2, 0), char, font=font, fill=1)

            pixels = img.load()
            for y in range(CHAR_HEIGHT):
                byte = 0
                for x in range(CHAR_WIDTH):
                    if pixels[x, y]:
                        byte |= (1 << (7 - x))
                f.write(bytes([byte]))

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: python3 gen_font.py <input.ttf> <output.bin>")
        sys.exit(1)
    generate_font_bin(sys.argv[1], sys.argv[2])