
import sys

def check_braces(filename):
    try:
        with open(filename, 'r', encoding='utf-8') as f:
            lines = f.readlines()
    except Exception as e:
        print(f"Error reading file: {e}")
        return

    stack = [] 

    print(f"Checking braces in {filename}...")

    for i, line in enumerate(lines):
        line_num = i + 1
        
        # Remove comments (simple)
        clean_line = line.split("//")[0] 
        
        for char in clean_line:
            if char == '{':
                stack.append(line_num)
            elif char == '}':
                if len(stack) > 0:
                    stack.pop()
                else:
                    print(f"ERROR: Extra closing brace at line {line_num}")

    if len(stack) > 0:
        print(f"ERROR: Unbalanced braces! {len(stack)} open braces remaining.")
        print("Unmatched open braces at lines:", stack[-5:]) # Print last 5
    else:
        print("Braces are balanced.")

if __name__ == "__main__":
    check_braces("hook/apis/dx12_hook.cpp")
