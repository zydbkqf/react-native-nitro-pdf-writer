import fs from 'fs';
import path from 'path';
import { execSync } from 'child_process';
import { fileURLToPath } from 'url';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

const ROOT = path.resolve(__dirname, '..');
const TESTS_DIR = path.join(ROOT, 'cpp', 'tests');
const BUILD_DIR = path.join(TESTS_DIR, 'build');

function run(command, cwd) {
  console.log(`[test-cpp] ${command}`);
  execSync(command, { cwd, stdio: 'inherit' });
}

if (!fs.existsSync(BUILD_DIR)) {
  fs.mkdirSync(BUILD_DIR, { recursive: true });
}

run('cmake -B build -S . -DCMAKE_POLICY_VERSION_MINIMUM=3.5', TESTS_DIR);
run('cmake --build build --parallel', TESTS_DIR);
run('ctest --test-dir build --output-on-failure', TESTS_DIR);
