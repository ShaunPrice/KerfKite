package au.cncstudio.pocket;

import android.content.Context;
import android.graphics.*;
import android.view.MotionEvent;
import android.view.View;
import java.util.function.IntConsumer;

final class BedView extends View {
    final Design model;
    final IntConsumer select;
    final Paint pen = new Paint(Paint.ANTI_ALIAS_FLAG);
    float left, bottom, scale;

    BedView(Context c, Design model, IntConsumer select) {
        super(c);
        this.model=model; this.select=select;
        setContentDescription("Drawing preview. Select a shape here or in the objects list.");
    }

    @Override protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        int w=getWidth(), h=getHeight();
        canvas.drawColor(0xff112b2c);
        double minX=0, minY=0, maxX=100, maxY=60;
        if(!model.parts.isEmpty()) {
            minX=680; minY=240; maxX=maxY=0;
            for(Part p:model.parts) {
                minX=Math.min(minX,p.x); minY=Math.min(minY,p.y);
                maxX=Math.max(maxX,p.x+p.width); maxY=Math.max(maxY,p.y+p.height);
            }
        }
        float padding=Math.min(w,h)*.12f;
        scale=(float)Math.min((w-2*padding)/(maxX-minX),(h-2*padding)/(maxY-minY));
        left=(float)(w/2.0-scale*(minX+maxX)/2);
        bottom=(float)(h/2.0+scale*(minY+maxY)/2);
        pen.setStyle(Paint.Style.STROKE);
        float grid=10*scale;
        if(grid>0) {
            while(grid<12)grid*=2;
            pen.setStrokeWidth(1); pen.setColor(0xff264344);
            for(float x=left%grid; x<w; x+=grid) canvas.drawLine(x,0,x,h,pen);
            for(float y=bottom%grid; y<h; y+=grid) canvas.drawLine(0,y,w,y,pen);
        }
        for(int i=0;i<model.parts.size();i++){
            Path path=new Path();
            double[][] points=model.parts.get(i).outline();
            for(int j=0;j<points.length;j++){
                float x=left+(float)points[j][0]*scale, y=bottom-(float)points[j][1]*scale;
                if(j==0)path.moveTo(x,y);else path.lineTo(x,y);
            }
            path.close();
            pen.setStrokeWidth(i==model.selected?4:2); pen.setColor(i==model.selected?0xff8aebce:0xff9ebabc);
            canvas.drawPath(path,pen);
        }
    }

    @Override public boolean onTouchEvent(MotionEvent event){
        if(event.getAction()==MotionEvent.ACTION_UP && scale>0) {
            double x=(event.getX()-left)/scale,y=(bottom-event.getY())/scale;
            for(int i=model.parts.size()-1;i>=0;i--){
                Part p=model.parts.get(i);
                if(x>=p.x && x<=p.x+p.width && y>=p.y && y<=p.y+p.height){select.accept(i); break;}
            }
            performClick();
        }
        return true;
    }
    @Override public boolean performClick(){super.performClick();return true;}
}
