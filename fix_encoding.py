import os

files = [
    r"hook/common/system_metrics.cpp",
    r"hook/common/system_metrics.h",
    r"hook/common/overlay.cpp",
    r"hook/common/overlay.h"
]

for f in files:
    if not os.path.exists(f):
        print(f"Skipping {f} (not found)")
        continue
        
    try:
        # Read as binary to check BOM
        with open(f, 'rb') as fp:
            content = fp.read()
        
        text = ""
        encoding_found = "unknown"
        
        if content.startswith(b'\xff\xfe'):
            text = content.decode('utf-16le')
            encoding_found = "UTF-16LE"
        elif content.startswith(b'\xfe\xff'):
            text = content.decode('utf-16be')
            encoding_found = "UTF-16BE"
        elif content.startswith(b'\xef\xbb\xbf'):
            text = content.decode('utf-8-sig')
            encoding_found = "UTF-8-BOM"
        else:
            # Try UTF-8
            try:
                text = content.decode('utf-8')
                encoding_found = "UTF-8"
            except:
                # Fallback to loose CP1252
                text = content.decode('cp1252', errors='replace')
                encoding_found = "CP1252"
        
        # Write back as clean UTF-8 (no BOM)
        with open(f, 'w', encoding='utf-8') as fp:
            fp.write(text)
            
        print(f"Processed {f}: Detected {encoding_found} -> Written as UTF-8")
        
    except Exception as e:
        print(f"Error processing {f}: {e}")
