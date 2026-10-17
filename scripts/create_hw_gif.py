import os
from PIL import Image

BASE_DIR = r"c:\Users\rahul\OpenCode\TuneBar"
DOC_DIR = os.path.join(BASE_DIR, "document")

files = [
    os.path.join(DOC_DIR, "hw_header.png"),
    os.path.join(DOC_DIR, "hw_overview.png"),
    os.path.join(DOC_DIR, "hw_size1.png"),
    os.path.join(DOC_DIR, "hw_size2.png")
]

images = [Image.open(f).convert("RGBA") for f in files]
sizes = [im.size for im in images]
print("Sizes:", sizes)

# Target unified canvas size (max width and height or standard aspect)
max_w = max(im.width for im in images)
max_h = max(im.height for im in images)

frames = []
for im in images:
    # Create white canvas matching hardware background
    bg = Image.new("RGBA", (max_w, max_h), (255, 255, 255, 255))
    offset = ((max_w - im.width) // 2, (max_h - im.height) // 2)
    bg.paste(im, offset, im if im.mode == 'RGBA' else None)
    # Convert to adaptive palette for GIF
    p_frame = bg.convert("RGB").convert("P", palette=Image.ADAPTIVE, colors=256)
    frames.append(p_frame)

out_gif = os.path.join(DOC_DIR, "hw_overview.gif")
durations = [2500, 2500, 2500, 2500]

frames[0].save(
    out_gif,
    save_all=True,
    append_images=frames[1:],
    duration=durations,
    loop=0,
    optimize=True
)

print(f"Created {out_gif} ({os.path.getsize(out_gif)} bytes)")
