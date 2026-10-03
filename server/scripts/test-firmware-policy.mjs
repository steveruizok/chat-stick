import { mkdtempSync, rmSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { execFileSync } from 'node:child_process'

const common = fileURLToPath(new URL('../../devices/firmware/common/', import.meta.url))
const temporary = mkdtempSync(join(tmpdir(), 'chat-stick-policy-'))
try {
  const binary = join(temporary, 'firmware-policy-test')
  execFileSync(process.env.CXX || 'c++', [
    '-std=c++17', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
    '-fno-omit-frame-pointer', `-I${join(common, 'src')}`,
    join(common, 'test/firmware_policy_test.cpp'), '-o', binary,
  ], { stdio: 'inherit' })
  execFileSync(binary, { stdio: 'inherit' })
} finally {
  rmSync(temporary, { recursive: true, force: true })
}
