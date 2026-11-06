/**
 * @file    mkimg.c
 * @brief   给纯代码 bin 追加 64 字节镜像元数据，生成发布包。
 *
 * 该工具只依赖 C 标准库，可在 Windows(MinGW/gcc) 与 Linux(gcc) 上直接编译，
 * 编译产物分别为 mkimg.exe 与 mkimg。
 *
 * 用法：
 *   mkimg <输入bin> <输出bin> [选项]
 *   选项：
 *     --version		镜像版本，缺省取编译期默认值
 *     --project		项目水印，缺省取编译期默认值
 *     --customer		客户编号，缺省取编译期默认值
 *     --batch			批次号，缺省取编译期默认值
 *     --build-date		构建日期，缺省取编译期默认值
 *     --help			打印帮助
 *
 * 镜像元数据紧贴代码末尾：输出文件 = 代码 + 64 字节元数据。
 * 字段布局与 CRC 算法必须与 library/src/fw_img.c 完全一致。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ==================== 编译期默认配置 ====================
 * 发行新版本时改这里并重新编译；命令行同名选项可临时覆盖，
 * 供不改代码的场合使用。
 */
#define META_VER_MAJOR      0       /*镜像版本主号，0~255*/
#define META_VER_MINOR      0       /*镜像版本次号，0~255*/
#define META_VER_PATCH      0       /*镜像版本修订号，0~255*/
#define META_PROJECT        0u      /*项目水印，必须与设备端 FW_IMG_PROJECT_ID 一致*/
#define META_CUSTOMER       0u      /*客户编号，0~65535*/
#define META_BATCH          0u      /*批次号，0~65535*/
#define META_BUILD_DATE     0u      /*构建日期，如20261020；取固定值可保证同样源码两次打包一致*/

/* ==================== 镜像元数据常量 ====================
 * 必须与 library/inc/fw_img.h 保持一致。
 */
#define META_MAGIC          0x48474D49u     /*魔数 'IMGH' 小端*/
#define META_FMT_VER        1u              /*镜像元数据格式版本*/
#define META_LEN            64u             /*镜像元数据总长度*/
#define META_CRC16_OFS      62u             /*元数据CRC16所在偏移，覆盖前62字节*/
#define META_CODE_CRC32_OFS 28u             /*代码CRC32所在偏移*/

/*单文件读取上限，防止误传超大文件把内存吃穿*/
#define MAX_INPUT_SIZE      (16u * 1024u * 1024u)

/**
 * @brief   计算 Modbus CRC16，初值 0xFFFF，多项式 0xA001，结果不取反
 * @param   data  数据指针
 * @param   len   数据长度
 * @return  CRC16 值
 */
static uint16_t crc16_modbus(const uint8_t *data, uint32_t len)
{
  uint16_t crc = 0xFFFFu;
  uint32_t i;
  uint8_t bit;

  for(i = 0u; i < len; i++)    //逐字节异或后按位处理
  {
    crc ^= data[i];
    for(bit = 0u; bit < 8u; bit++)
    {
      if((crc & 1u) != 0u)
      {
        crc = (uint16_t)((crc >> 1) ^ 0xA001u);
      }
      else
      {
        crc = (uint16_t)(crc >> 1);
      }
    }
  }
  return crc;
}

/**
 * @brief   计算标准反射 CRC32 的增量值
 * @param   crc   上一轮的 CRC 值，首次传入 0xFFFFFFFF
 * @param   data  数据指针
 * @param   len   数据长度
 * @return  累加后的 CRC 值，最终结果需再与 0xFFFFFFFF 异或
 */
static uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t len)
{
  uint32_t i;
  uint8_t bit;

  for(i = 0u; i < len; i++)    //逐字节异或后按位处理
  {
    crc ^= data[i];
    for(bit = 0u; bit < 8u; bit++)
    {
      if((crc & 1u) != 0u)
      {
        crc = (crc >> 1) ^ 0xEDB88320u;    //反射多项式
      }
      else
      {
        crc >>= 1;
      }
    }
  }
  return crc;
}

/**
 * @brief   按小端写入 16 位整数
 * @param   buf  目标缓冲区
 * @param   ofs  写入偏移
 * @param   val  待写入的值
 */
static void put_u16(uint8_t *buf, uint32_t ofs, uint16_t val)
{
  buf[ofs + 0u] = (uint8_t)(val & 0xFFu);
  buf[ofs + 1u] = (uint8_t)((val >> 8) & 0xFFu);
}

/**
 * @brief   按小端写入 32 位整数
 * @param   buf  目标缓冲区
 * @param   ofs  写入偏移
 * @param   val  待写入的值
 */
static void put_u32(uint8_t *buf, uint32_t ofs, uint32_t val)
{
  buf[ofs + 0u] = (uint8_t)(val & 0xFFu);
  buf[ofs + 1u] = (uint8_t)((val >> 8) & 0xFFu);
  buf[ofs + 2u] = (uint8_t)((val >> 16) & 0xFFu);
  buf[ofs + 3u] = (uint8_t)((val >> 24) & 0xFFu);
}

/**
 * @brief   按小端读取 32 位整数
 * @param   buf  数据指针
 * @param   ofs  读取偏移
 * @return  读到的值
 */
static uint32_t get_u32(const uint8_t *buf, uint32_t ofs)
{
  return (uint32_t)buf[ofs + 0u]
       | ((uint32_t)buf[ofs + 1u] << 8)
       | ((uint32_t)buf[ofs + 2u] << 16)
       | ((uint32_t)buf[ofs + 3u] << 24);
}

/**
 * @brief   生成 64 字节镜像元数据
 * @param   out          输出缓冲区，至少 64 字节
 * @param   code_len     代码长度，不含元数据
 * @param   image_ver    镜像版本，由主次修订三段打包
 * @param   project_id   项目水印
 * @param   customer_id  客户编号
 * @param   batch_id     批次号
 * @param   build_date   构建日期
 * @param   code_crc32   代码的最终 CRC32
 */
static void build_meta(uint8_t *out, uint32_t code_len, uint32_t image_ver,
                       uint32_t project_id, uint16_t customer_id, uint16_t batch_id,
                       uint32_t build_date, uint32_t code_crc32)
{
  memset(out, 0, META_LEN);    //保留字段一律清零，避免带出未初始化内容
  put_u32(out, 0u, META_MAGIC);
  out[4] = (uint8_t)META_FMT_VER;
  out[5] = 0u;
  put_u16(out, 6u, (uint16_t)META_LEN);
  put_u32(out, 8u, code_len + META_LEN);    //image_len 是逻辑总长，含元数据
  put_u32(out, 12u, image_ver);
  put_u32(out, 16u, project_id);
  put_u16(out, 20u, customer_id);
  put_u16(out, 22u, batch_id);
  put_u32(out, 24u, build_date);
  put_u32(out, META_CODE_CRC32_OFS, code_crc32);
  //偏移 32..61 的 30 字节保留区保持为 0
  put_u16(out, META_CRC16_OFS, crc16_modbus(out, META_CRC16_OFS));    //最后算元数据自身校验
}

/**
 * @brief   把三段版本号打包成 32 位整数
 * @param   major  主版本号，0~255
 * @param   minor  次版本号，0~255
 * @param   patch  修订号，0~255
 * @return  打包后的版本值
 */
static uint32_t pack_version(uint32_t major, uint32_t minor, uint32_t patch)
{
  return ((major & 0xFFu) << 24) | ((minor & 0xFFu) << 16) | ((patch & 0xFFu) << 8);
}

/**
 * @brief   解析三段版本号字符串
 * @param   text   形如 "1.0.3" 的字符串
 * @param   out    输出：打包后的版本值
 * @return  0成功，非0格式错误
 */
static int parse_version(const char *text, uint32_t *out)
{
  unsigned int major = 0u;
  unsigned int minor = 0u;
  unsigned int patch = 0u;
  char tail = 0;

  if(sscanf(text, "%3u.%3u.%3u%c", &major, &minor, &patch, &tail) != 3)
  {
    return -1;
  }
  if(major > 255u || minor > 255u || patch > 255u)
  {
    return -1;
  }
  *out = pack_version(major, minor, patch);
  return 0;
}

/**
 * @brief   把版本值拆成便于阅读的三段并格式化
 * @param   ver  打包后的版本值
 * @param   buf  输出缓冲区
 * @param   len  缓冲区长度
 */
static void format_version(uint32_t ver, char *buf, size_t len)
{
  snprintf(buf, len, "%u.%u.%u",
           (unsigned)((ver >> 24) & 0xFFu),
           (unsigned)((ver >> 16) & 0xFFu),
           (unsigned)((ver >> 8) & 0xFFu));
}

/**
 * @brief   读取整个文件到内存
 * @param   path    文件路径
 * @param   out_buf 输出：malloc 分配的缓冲区，调用方负责 free
 * @param   out_len 输出：文件长度
 * @return  0成功，非0失败
 */
static int read_file(const char *path, uint8_t **out_buf, uint32_t *out_len)
{
  FILE *fp;
  long size;
  uint8_t *buf;

  fp = fopen(path, "rb");
  if(fp == NULL)
  {
    fprintf(stderr, "error: cannot open input file: %s\n", path);
    return -1;
  }
  if(fseek(fp, 0L, SEEK_END) != 0)
  {
    fprintf(stderr, "error: cannot seek input file\n");
    fclose(fp);
    return -1;
  }
  size = ftell(fp);
  if(size < 0L)
  {
    fprintf(stderr, "error: cannot get input file size\n");
    fclose(fp);
    return -1;
  }
  if(size == 0L || (unsigned long)size > MAX_INPUT_SIZE)
  {
    fprintf(stderr, "error: input size out of range (%ld bytes)\n", size);
    fclose(fp);
    return -1;
  }
  rewind(fp);
  buf = (uint8_t *)malloc((size_t)size);
  if(buf == NULL)
  {
    fprintf(stderr, "error: out of memory\n");
    fclose(fp);
    return -1;
  }
  if(fread(buf, 1u, (size_t)size, fp) != (size_t)size)
  {
    fprintf(stderr, "error: cannot read input file\n");
    free(buf);
    fclose(fp);
    return -1;
  }
  fclose(fp);
  *out_buf = buf;
  *out_len = (uint32_t)size;
  return 0;
}

/**
 * @brief   把缓冲区写入文件
 * @param   path  文件路径
 * @param   buf   数据指针
 * @param   len   数据长度
 * @return  0成功，非0失败
 */
static int write_file(const char *path, const uint8_t *buf, uint32_t len)
{
  FILE *fp;
  size_t written;

  fp = fopen(path, "wb");
  if(fp == NULL)
  {
    fprintf(stderr, "error: cannot create output file: %s\n", path);
    return -1;
  }
  written = fwrite(buf, 1u, (size_t)len, fp);
  if(written != (size_t)len)
  {
    fprintf(stderr, "error: cannot write output file\n");
    fclose(fp);
    return -1;
  }
  if(fclose(fp) != 0)
  {
    fprintf(stderr, "error: cannot flush output file\n");
    return -1;
  }
  return 0;
}

/**
 * @brief   打印帮助
 * @param   prog  程序名
 */
static void print_help(const char *prog)
{
  printf("usage: %s <input.bin> <output.pkg.bin> [options]\n", prog);
  printf("  --version x.y.z   image version, default %u.%u.%u\n",
         (unsigned)META_VER_MAJOR, (unsigned)META_VER_MINOR, (unsigned)META_VER_PATCH);
  printf("  --project N       project watermark, default %u\n", (unsigned)META_PROJECT);
  printf("  --customer N      customer id, default %u\n", (unsigned)META_CUSTOMER);
  printf("  --batch N         batch id, default %u\n", (unsigned)META_BATCH);
  printf("  --build-date N    build date, default %u\n", (unsigned)META_BUILD_DATE);
  printf("  --help            show this message\n");
}

/**
 * @brief   把命令行参数解析成无符号整数
 * @param   text  十进制字符串
 * @param   out   输出值
 * @return  0成功，非0格式错误
 */
static int parse_u32(const char *text, uint32_t *out)
{
  char *end = NULL;
  unsigned long val;

  if(text == NULL || text[0] == '\0')
  {
    return -1;
  }
  val = strtoul(text, &end, 10);
  if(end == NULL || *end != '\0' || val > 0xFFFFFFFFul)
  {
    return -1;
  }
  *out = (uint32_t)val;
  return 0;
}

/**
 * @brief   程序入口
 * @param   argc  参数个数
 * @param   argv  参数数组
 * @return  0成功，非0失败
 */
int main(int argc, char *argv[])
{
  const char *in_path = NULL;
  const char *out_path = NULL;
  uint32_t image_ver = pack_version(META_VER_MAJOR, META_VER_MINOR, META_VER_PATCH);
  uint32_t project = META_PROJECT;
  uint32_t customer = META_CUSTOMER;
  uint32_t batch = META_BATCH;
  uint32_t build_date = META_BUILD_DATE;
  uint8_t *code = NULL;
  uint32_t code_len = 0u;
  uint32_t crc;
  uint8_t meta[META_LEN];
  uint8_t *pkg;
  uint32_t pkg_len;
  char ver_text[24];
  int i;

  //先扫一遍参数，允许选项出现在文件名前后
  for(i = 1; i < argc; i++)
  {
    if(strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
    {
      print_help(argv[0]);
      return 0;
    }
    else if(strcmp(argv[i], "--version") == 0 && (i + 1) < argc)
    {
      if(parse_version(argv[++i], &image_ver) != 0)
      {
        fprintf(stderr, "error: bad version, expected x.y.z with each part 0..255\n");
        return 1;
      }
    }
    else if(strcmp(argv[i], "--project") == 0 && (i + 1) < argc)
    {
      if(parse_u32(argv[++i], &project) != 0)
      {
        fprintf(stderr, "error: bad project value\n");
        return 1;
      }
    }
    else if(strcmp(argv[i], "--customer") == 0 && (i + 1) < argc)
    {
      if(parse_u32(argv[++i], &customer) != 0 || customer > 0xFFFFu)
      {
        fprintf(stderr, "error: bad customer value\n");
        return 1;
      }
    }
    else if(strcmp(argv[i], "--batch") == 0 && (i + 1) < argc)
    {
      if(parse_u32(argv[++i], &batch) != 0 || batch > 0xFFFFu)
      {
        fprintf(stderr, "error: bad batch value\n");
        return 1;
      }
    }
    else if(strcmp(argv[i], "--build-date") == 0 && (i + 1) < argc)
    {
      if(parse_u32(argv[++i], &build_date) != 0)
      {
        fprintf(stderr, "error: bad build date value\n");
        return 1;
      }
    }
    else if(argv[i][0] == '-')
    {
      fprintf(stderr, "error: unknown option: %s\n", argv[i]);
      print_help(argv[0]);
      return 1;
    }
    else if(in_path == NULL)
    {
      in_path = argv[i];
    }
    else if(out_path == NULL)
    {
      out_path = argv[i];
    }
    else
    {
      fprintf(stderr, "error: unexpected argument: %s\n", argv[i]);
      return 1;
    }
  }

  if(in_path == NULL || out_path == NULL)
  {
    print_help(argv[0]);
    return 1;
  }

  if(read_file(in_path, &code, &code_len) != 0)
  {
    return 1;
  }

  //输入若已自带元数据则拒绝，避免把发布包再套一层
  if(code_len >= META_LEN && get_u32(code, code_len - META_LEN) == META_MAGIC)
  {
    fprintf(stderr, "error: input already carries image metadata, pass the pure code bin\n");
    free(code);
    return 1;
  }

  crc = crc32_update(0xFFFFFFFFu, code, code_len) ^ 0xFFFFFFFFu;    //代码CRC32，只覆盖代码
  build_meta(meta, code_len, image_ver, project, (uint16_t)customer, (uint16_t)batch, build_date, crc);

  pkg_len = code_len + META_LEN;
  pkg = (uint8_t *)malloc((size_t)pkg_len);
  if(pkg == NULL)
  {
    fprintf(stderr, "error: out of memory\n");
    free(code);
    return 1;
  }
  memcpy(pkg, code, code_len);                   //代码原样保留
  memcpy(pkg + code_len, meta, META_LEN);        //元数据紧贴代码末尾
  free(code);

  if(write_file(out_path, pkg, pkg_len) != 0)
  {
    free(pkg);
    return 1;
  }

  //回读校验，确认落盘内容与内存一致
  free(pkg);
  {
    uint8_t *check = NULL;
    uint32_t check_len = 0u;
    if(read_file(out_path, &check, &check_len) != 0)
    {
      return 1;
    }
    if(check_len != pkg_len
       || get_u32(check, check_len - META_LEN) != META_MAGIC
       || crc16_modbus(check + check_len - META_LEN, META_CRC16_OFS)
          != (uint16_t)(check[check_len - 2u] | ((uint16_t)check[check_len - 1u] << 8)))
    {
      fprintf(stderr, "error: output self-check failed\n");
      free(check);
      return 1;
    }
    free(check);
  }

  format_version(image_ver, ver_text, sizeof(ver_text));
  printf("code        : %u bytes\n", (unsigned)code_len);
  printf("image total : %u bytes (code + %u)\n", (unsigned)pkg_len, (unsigned)META_LEN);
  printf("version     : %s (0x%08X)\n", ver_text, (unsigned)image_ver);
  printf("project     : %u\n", (unsigned)project);
  printf("customer    : %u\n", (unsigned)customer);
  printf("batch       : %u\n", (unsigned)batch);
  printf("build date  : %u\n", (unsigned)build_date);
  printf("code crc32  : 0x%08X\n", (unsigned)crc);
  printf("meta crc16  : 0x%04X\n", (unsigned)crc16_modbus(meta, META_CRC16_OFS));
  printf("output      : %s\n", out_path);
  return 0;
}
