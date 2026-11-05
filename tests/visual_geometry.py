"""Measure native screenshot geometry using the existing ImageMagick dependency.

These checks protect the October reference's frame and prompt placement. They do
not assert that different conversation text, fonts, or every menu match pixels.
"""
import subprocess


def measure_frame(path, executable, sidebar=True, prompt_height=44, column_limit=768, panel=0):
    width, height = map(int, subprocess.check_output(
        [executable("identify"), "-format", "%w %h", str(path)], text=True
    ).split())
    pixels = subprocess.check_output([
        executable("convert"), str(path), "-alpha", "off", "-depth", "8", "rgb:-"
    ])

    def pixel(x, y):
        offset = (y * width + x) * 3
        return tuple(pixels[offset:offset + 3])

    sidebar_width = 280 if sidebar else 0
    if sidebar:
        assert pixel(4, height // 2) == (31, 31, 31), "Sidebar palette drifted"
        assert pixel(278, height // 2) == (31, 31, 31), "Sidebar width drifted"
    assert pixel(sidebar_width + 8, height // 2) == (25, 25, 25), "Conversation palette drifted"
    main_right = width - panel
    center_x = (sidebar_width + main_right) // 2
    runs = []
    for y in range(height - 150, height):
        if pixel(center_x, y) == (48, 48, 48):
            if runs and runs[-1][-1] == y - 1:
                runs[-1].append(y)
            else:
                runs.append([y])
    assert runs, "Prompt surface is absent or has the wrong color"
    run = max(runs, key=len)
    prompt_y = run[0] - 1
    scan_y = (run[0] + run[-1]) // 2
    filled = [x for x in range(sidebar_width, main_right) if pixel(x, scan_y) == (48, 48, 48)]
    prompt_x = min(filled) - 1
    measured_width = max(filled) - min(filled) + 3
    measured_height = len(run) + 2
    expected_width = min(column_limit, main_right - sidebar_width - 64)
    assert measured_width == expected_width, (path.name, "prompt width", measured_width, expected_width)
    assert prompt_x == (main_right + sidebar_width - expected_width) // 2, (path.name, "prompt centering", prompt_x)
    assert measured_height == prompt_height, (path.name, "prompt height", measured_height, prompt_height)
    return {
        "window": [width, height], "sidebar_width": sidebar_width,
        "prompt": [prompt_x, prompt_y, measured_width, measured_height],
    }
