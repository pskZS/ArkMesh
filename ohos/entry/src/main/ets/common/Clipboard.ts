// 复制到系统剪贴板。IP / 域名这类值用户几乎总是要拿去别处粘贴，
// 长按选择在小屏上很别扭，所以给一个明确的复制入口。
import { pasteboard } from '@kit.BasicServicesKit'

export function copyText(text: string): Promise<void> {
  if (text === '') {
    return Promise.resolve()
  }
  try {
    const data = pasteboard.createData(pasteboard.MIMETYPE_TEXT_PLAIN, text)
    return pasteboard.getSystemPasteboard().setData(data)
  } catch (err) {
    return Promise.reject(err as Error)
  }
}
