import sys
import os
from PIL import Image, ImageFont, ImageDraw

def generate_font_bin(ttf_path, output_path, char_width=8, char_height=16, font_size=14):
    if not os.path.exists(ttf_path):
        print(f"Error: Font file {ttf_path} not found.")
        sys.exit(1)

    try:
        font = ImageFont.truetype(ttf_path, font_size)
    except Exception as e:
        print(f"Error loading font: {e}")
        sys.exit(1)

    with open(output_path, "wb") as f:
        for i in range(256):
            img = Image.new('1', (char_width, char_height), 0)
            draw = ImageDraw.Draw(img)
            
            char = chr(i)
            
            bbox = draw.textbbox((0, 0), char, font=font)
            w = bbox[2] - bbox[0]
            h = bbox[3] - bbox[1]
            
            off_x = (char_width - w) // 2 - bbox[0]
            off_y = (char_height - h) // 2 - bbox[1]
            
            draw.text((off_x, off_y), char, font=font, fill=1)

            pixels = img.load()
            for y in range(char_height):
                byte = 0
                for x in range(char_width):
                    if pixels[x, y]:
                        byte |= (1 << (7 - x))
                f.write(bytes([byte]))

    print(f"Successfully generated {output_path} ({256 * char_height} bytes)")

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: python3 gen_font.py <input.ttf> <output.bin> [font_size]")
        sys.exit(1)
    
    f_size = int(sys.argv[3]) if len(sys.argv) > 3 else 14
    generate_font_bin(sys.argv[1], sys.argv[2], font_size=f_size)