/**
 * @file    fw_delta.c
 * @brief   差分记录流解码器(COPY/LITERAL)
 * @note    只做流式还原，不依赖Flash、镜像元数据、CRC32与升级协议。
 *          旧数据经read回调取，新数据经write回调按顺序交出。
 */

#include "fw_delta.h"
#include <string.h>

/**
* @brief   读小端16位
* @param   p  数据指针
* @return  读到的值
*/
static uint16_t fw_delta_rd_u16(const uint8_t *p)
{
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/**
* @brief   读小端32位
* @param   p  数据指针
* @return  读到的值
*/
static uint32_t fw_delta_rd_u32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/**
* @brief   把输出缓冲交给write回调
* @param   ctx  解码器上下文
* @return  FW_DELTA_OK或负错误码
*/
static int fw_delta_flush(fw_delta_ctx_t *ctx)
{
  if(ctx->out_fill == 0u)
  {
    return FW_DELTA_OK;
  }
  if(ctx->write(ctx->user, ctx->out_len, ctx->buf, ctx->out_fill) != 0)
  {
    return FW_DELTA_ERR_IO;
  }
  ctx->out_len += ctx->out_fill;
  ctx->out_fill = 0u;
  return FW_DELTA_OK;
}

/**
* @brief   初始化解码器
* @param   ctx       解码器上下文
* @param   code_len  目标代码总长
* @param   old_len   旧代码总长
* @param   read      读旧镜像回调
* @param   write     写新镜像回调
* @param   user      回调上下文
* @return  FW_DELTA_OK或负错误码
*/
int fw_delta_init(fw_delta_ctx_t *ctx, uint32_t code_len, uint32_t old_len,
                  fw_delta_read_fn read, fw_delta_write_fn write, void *user)
{
  if(ctx == NULL || read == NULL || write == NULL || code_len == 0u)
  {
    return FW_DELTA_ERR_PARAM;
  }
  memset(ctx, 0, sizeof(*ctx));
  ctx->code_len = code_len;
  ctx->old_len = old_len;
  ctx->read = read;
  ctx->write = write;
  ctx->user = user;
  return FW_DELTA_OK;
}

/**
* @brief   解析刚收满的记录头并做边界检查
* @param   ctx  解码器上下文
* @return  FW_DELTA_OK或负错误码
*/
static int fw_delta_parse_hdr(fw_delta_ctx_t *ctx)
{
  if(ctx->op == FW_DELTA_OP_COPY)
  {
    ctx->src = fw_delta_rd_u32(ctx->hdr);
    ctx->remain = fw_delta_rd_u32(ctx->hdr + 4u);
    if(ctx->remain == 0u)
    {
      return FW_DELTA_ERR_FORMAT;
    }
    //源区间必须在旧镜像内，用减法避免src+remain溢出
    if(ctx->src > ctx->old_len || ctx->remain > (ctx->old_len - ctx->src))
    {
      return FW_DELTA_ERR_RANGE;
    }
  }
  else if(ctx->op == FW_DELTA_OP_LITERAL)
  {
    ctx->remain = fw_delta_rd_u32(ctx->hdr);
    if(ctx->remain == 0u)
    {
      return FW_DELTA_ERR_FORMAT;
    }
  }
  else
  {
    //DIFF：src + len + count，块长受输出缓冲限制
    ctx->src = fw_delta_rd_u32(ctx->hdr);
    ctx->remain = fw_delta_rd_u32(ctx->hdr + 4u);
    ctx->diff_left = fw_delta_rd_u16(ctx->hdr + 8u);
    ctx->diff_started = 0u;
    ctx->diff_have = 0u;
    if(ctx->remain == 0u || ctx->remain > FW_DELTA_BUF_SIZE)
    {
      return FW_DELTA_ERR_FORMAT;
    }
    //count为0时diff_left不会归零，会让本记录永远结束不了，直接判格式错
    if(ctx->diff_left == 0u || ctx->diff_left > ctx->remain)
    {
      return FW_DELTA_ERR_FORMAT;
    }
    if(ctx->src > ctx->old_len || ctx->remain > (ctx->old_len - ctx->src))
    {
      return FW_DELTA_ERR_RANGE;
    }
  }
  //当前记录连同缓冲里未交出的数据，都不能超过目标代码长度
  {
    uint32_t used = ctx->out_len + (uint32_t)ctx->out_fill;
    if(used > ctx->code_len || ctx->remain > (ctx->code_len - used))
    {
      return FW_DELTA_ERR_RANGE;
    }
  }
  return FW_DELTA_OK;
}

/**
* @brief   喂入一段差分流
* @param   ctx   解码器上下文
* @param   data  差分流字节
* @param   len   字节数
* @return  FW_DELTA_OK或负错误码
* @note    记录头与记录内容都允许跨片；COPY按缓冲空间分批从旧镜像读取。
*/
int fw_delta_feed(fw_delta_ctx_t *ctx, const uint8_t *data, uint32_t len)
{
  int rc;

  if(ctx == NULL || (data == NULL && len > 0u))
  {
    return FW_DELTA_ERR_PARAM;
  }
  for(;;)
  {
    //1.取记录类型(需要输入)
    if(ctx->op == 0u)
    {
      uint8_t op;
      if(len == 0u)
      {
        break;
      }
      op = *data++;
      len--;
      if(op == FW_DELTA_OP_COPY)
      {
        ctx->hdr_need = 8u;
      }
      else if(op == FW_DELTA_OP_LITERAL)
      {
        ctx->hdr_need = 4u;
      }
      else if(op == FW_DELTA_OP_DIFF)
      {
        ctx->hdr_need = 10u;
      }
      else
      {
        return FW_DELTA_ERR_FORMAT;
      }
      ctx->op = op;
      ctx->hdr_have = 0u;
      continue;
    }
    //2.收记录头(需要输入)
    if(ctx->hdr_have < ctx->hdr_need)
    {
      if(len == 0u)
      {
        break;
      }
      ctx->hdr[ctx->hdr_have] = *data++;
      len--;
      ctx->hdr_have++;
      if(ctx->hdr_have < ctx->hdr_need)
      {
        continue;
      }
      rc = fw_delta_parse_hdr(ctx);
      if(rc != FW_DELTA_OK)
      {
        return rc;
      }
      continue;
    }
    //3.产出一条记录的内容
    if(ctx->op == FW_DELTA_OP_LITERAL)
    {
      //LITERAL需要输入字节；没有输入就等下一次
      if(len == 0u)
      {
        break;
      }
      ctx->buf[ctx->out_fill] = *data++;
      len--;
      ctx->out_fill++;
      ctx->remain--;
    }
    else if(ctx->op == FW_DELTA_OP_COPY)
    {
      //COPY直接从旧镜像取数据，不需要输入字节
      uint32_t space = FW_DELTA_BUF_SIZE - (uint32_t)ctx->out_fill;
      uint32_t n = ((uint32_t)ctx->remain < space) ? (uint32_t)ctx->remain : space;

      //缓冲满时先交出一批，保证下面有空间
      if(n == 0u)
      {
        rc = fw_delta_flush(ctx);
        if(rc != FW_DELTA_OK)
        {
          return rc;
        }
        continue;
      }
      if(ctx->read(ctx->user, ctx->src, ctx->buf + ctx->out_fill, n) != 0)
      {
        return FW_DELTA_ERR_IO;
      }
      ctx->src += n;
      ctx->out_fill = (uint16_t)(ctx->out_fill + n);
      ctx->remain -= n;
    }
    else
    {
      //DIFF：先把旧块读进缓冲，再按稀疏差异逐点修改
      if(ctx->diff_started == 0u)
      {
        //上一记录没交出的数据先交出去
        if(ctx->out_fill != 0u)
        {
          rc = fw_delta_flush(ctx);
          if(rc != FW_DELTA_OK)
          {
            return rc;
          }
        }
        if(ctx->read(ctx->user, ctx->src, ctx->buf, ctx->remain) != 0)
        {
          return FW_DELTA_ERR_IO;
        }
        ctx->out_fill = (uint16_t)ctx->remain;
        ctx->diff_started = 1u;
        ctx->diff_have = 0u;
        continue;
      }
      if(len == 0u)
      {
        break;
      }
      ctx->diff_hdr[ctx->diff_have] = *data++;
      len--;
      ctx->diff_have++;
      if(ctx->diff_have < 3u)
      {
        continue;
      }
      {
        uint16_t pos = fw_delta_rd_u16(ctx->diff_hdr);
        uint8_t val = ctx->diff_hdr[2];

        if((uint32_t)pos >= (uint32_t)ctx->out_fill)
        {
          return FW_DELTA_ERR_RANGE;
        }
        ctx->buf[pos] = (uint8_t)(ctx->buf[pos] + val);
        //diff_left为0时自减会回绕成65535，此处兜底拒绝
        if(ctx->diff_left == 0u)
        {
          return FW_DELTA_ERR_FORMAT;
        }
        ctx->diff_left--;
        ctx->diff_have = 0u;
        if(ctx->diff_left == 0u)
        {
          rc = fw_delta_flush(ctx);
          if(rc != FW_DELTA_OK)
          {
            return rc;
          }
          ctx->op = 0u;
          ctx->remain = 0u;
          ctx->diff_started = 0u;
        }
      }
      continue;
    }
    //4.记录结束则回到空闲，缓冲满则交出一批
    if(ctx->remain == 0u)
    {
      ctx->op = 0u;
    }
    if(ctx->out_fill == FW_DELTA_BUF_SIZE)
    {
      rc = fw_delta_flush(ctx);
      if(rc != FW_DELTA_OK)
      {
        return rc;
      }
    }
  }
  return FW_DELTA_OK;
}

/**
* @brief   结束差分流并检查完整性
* @param   ctx  解码器上下文
* @return  FW_DELTA_OK或负错误码
*/
int fw_delta_finish(fw_delta_ctx_t *ctx)
{
  int rc;

  if(ctx == NULL)
  {
    return FW_DELTA_ERR_PARAM;
  }
  //把输入耗尽时尚未产出的COPY记录冲刷完
  rc = fw_delta_feed(ctx, NULL, 0u);
  if(rc != FW_DELTA_OK)
  {
    return rc;
  }
  //还有未完成的记录或未解析的记录头
  if(ctx->op != 0u || ctx->remain != 0u || ctx->diff_left != 0u
     || ctx->diff_started != 0u || ctx->diff_have != 0u
     || ctx->hdr_have != ctx->hdr_need)
  {
    return FW_DELTA_ERR_INCOMPLETE;
  }
  rc = fw_delta_flush(ctx);
  if(rc != FW_DELTA_OK)
  {
    return rc;
  }
  if(ctx->out_len != ctx->code_len)
  {
    return FW_DELTA_ERR_INCOMPLETE;
  }
  return FW_DELTA_OK;
}

/**
* @brief   查询已重建的代码长度
* @param   ctx  解码器上下文
* @return  已重建字节数(含尚未交给write回调的缓冲数据)
*/
uint32_t fw_delta_output_len(const fw_delta_ctx_t *ctx)
{
  if(ctx == NULL)
  {
    return 0u;
  }
  return ctx->out_len + (uint32_t)ctx->out_fill;
}
