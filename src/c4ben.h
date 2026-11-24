/* Converter4BENeXt —— C ABI（供 BEN/ForTMR4A 导入链复用）
 *
 * 上游：Converter4BENeXt（AGPL-3.0-or-later），vendored 到本仓库以便在 Android 上
 * 进程内调用；CLI 行为不变。调用方负责提供 mod 目录、BEN catalog 与输出路径。
 */
#ifndef C4BEN_H
#define C4BEN_H

#ifdef __cplusplus
extern "C" {
#endif

#define C4BEN_OK 0
#define C4BEN_ERR 1
#define C4BEN_BAD_ARG 2
#define C4BEN_UNSUPPORTED 3

/* 探测 mod 目录：0 未知，1 EFMI，2 EFF format 3。 */
int c4ben_probe(const char *dir);

/* 转换到 BEM（.bem）或 ZIP 容器（.zip）。错误文本写入 err。 */
int c4ben_convert(const char *dir, const char *catalog_dir, const char *out_path,
                  char *err, unsigned err_cap);

/* 旧 EFMI + mapping.json（--profile）路径。 */
int c4ben_convert_profile(const char *dir, const char *catalog_dir, const char *profile,
                          const char *out_path, int allow_review, char *err, unsigned err_cap);

int c4ben_version(void);

#ifdef __cplusplus
}
#endif
#endif /* C4BEN_H */
