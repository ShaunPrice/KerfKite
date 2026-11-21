package au.cncstudio.pocket;

import android.content.Context;
import android.graphics.*;
import android.view.MotionEvent;
import android.view.View;
import java.util.function.IntConsumer;

final class BedView extends View {
    Design model;
    final IntConsumer select;
    final Paint pen = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path outline = new Path(); // Reused for every shape: nothing is allocated while drawing.
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
            minX=model.width; minY=model.height; maxX=maxY=0;
            for(Part p:model.parts) {
                minX=Math.min(minX,p.x); minY=Math.min(minY,p.y);
                maxX=Math.max(maxX,p.x+p.width); maxY=Math.max(maxY,p.y+p.height);
            }
        }
        float padding=Math.min(w,h)*.12f;
        scale=(float)Math.min((w-2*padding)/Math.max(.2,maxX-minX),(h-2*padding)/Math.max(.2,maxY-minY));
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
        // The selected shape is drawn last so its highlight is not hidden by a shape stacked on top of it.
        for(int k=0;k<=model.parts.size();k++){
            int i=k<model.parts.size()?k:model.selected;
            if(i<0 || i>=model.parts.size() || (k<model.parts.size() && i==model.selected))continue;
            Path path=outline;
            path.rewind();
            for(double[][] points:model.parts.get(i).paths()){
            for(int j=0;j<points.length;j++){
                float x=left+(float)points[j][0]*scale, y=bottom-(float)points[j][1]*scale;
                if(j==0)path.moveTo(x,y);else path.lineTo(x,y);
            }
            }
            pen.setStrokeWidth(i==model.selected?4:2); pen.setColor(i==model.selected?0xff8aebce:0xff9ebabc);
            canvas.drawPath(path,pen);
        }
    }

    @Override public boolean onTouchEvent(MotionEvent event){
        if(event.getAction()==MotionEvent.ACTION_UP && scale>0) {
            double x=(event.getX()-left)/scale,y=(bottom-event.getY())/scale;
            // A fingertip or pen rarely lands exactly on an outline: allow 12 dp, expressed in millimetres.
            double tolerance=12*getResources().getDisplayMetrics().density/scale;
            int hit=Picking.pick(model.boxes(),model.selected,x,y,tolerance);
            if(hit>=0)select.accept(hit);
            performClick();
        }
        return true;
    }
    @Override public boolean performClick(){super.performClick();return true;}
}
